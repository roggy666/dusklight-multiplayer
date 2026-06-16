// Dusklight multiplayer client transport (Path B: native state-sync).
//
// Socket boilerplate mirrors src/dusk/livesplit.cpp (proven cross-platform
// non-blocking TCP). The wire protocol matches tools/mp_server/relay_server.py:
// every message is framed as [u16 little-endian length] followed by that many
// payload bytes; payload byte 0 is the message type. All ints little-endian.

#if _WIN32
    #ifndef NOMINMAX
    #define NOMINMAX
    #endif
    #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
    #endif
    #include <winsock2.h>
    #include <ws2tcpip.h>
    using socket_t = SOCKET;
    static void closeSocket(socket_t s) {
        LINGER li{1, 0};
        setsockopt(s, SOL_SOCKET, SO_LINGER, reinterpret_cast<const char*>(&li), sizeof(li));
        closesocket(s);
    }
    static constexpr int kSendFlags = 0;
#else
    #include <sys/socket.h>
    #include <sys/select.h>
    #include <netinet/in.h>
    #include <netinet/tcp.h>
    #include <arpa/inet.h>
    #include <unistd.h>
    #include <fcntl.h>
    #include <errno.h>
    using socket_t = int;
    static void closeSocket(socket_t s) {
        struct linger li{1, 0};
        setsockopt(s, SOL_SOCKET, SO_LINGER, &li, sizeof(li));
        close(s);
    }
    #ifndef INVALID_SOCKET
        #define INVALID_SOCKET (-1)
    #endif
    #if defined(__APPLE__)
        static constexpr int kSendFlags = 0;
    #else
        static constexpr int kSendFlags = MSG_NOSIGNAL;
    #endif
#endif

#include "dusk/net.h"
#include "dusk/logging.h"

// Game-side accessors for the local player's transform.
#include "d/actor/d_a_player.h"   // daPy_getLinkPlayerActorClass()
#include "d/actor/d_a_alink.h"    // daAlink_c::getDuskBodyAnmIdx() (current body anim)
#include "d/actor/d_a_horse.h"    // daHorse_c (shared-Epona anim/seat sync)
#include "m_Do/m_Do_controller_pad.h"  // mDoCPd_c (passenger board/dismount input)
#include "m_Do/m_Do_mtx.h"             // mDoMtx_multVec (passenger seat transform)
#include "d/d_com_inf_game.h"     // dComIfGp_roomControl_getStayNo(), getStartStageName(),
                                  // dComIfGs_getSaveData()/getRupee()/setRupee() (progress sync)
#include "f_op/f_op_overlap_mng.h"  // fopOvlpM_IsDoingReq() (don't touch the save mid-stream)

#include <cstdio>
#include <cstring>
#include <vector>

namespace dusk::net {
namespace {

enum class Msg : uint8_t {
    Hello        = 0,  // C->S: u8 version, u8 nameLen, name
    Welcome      = 1,  // S->C: u8 id
    State        = 2,  // C->S: u32 scene, s8 room, 3f pos, s16 angleY, u16 anim, ...
    Snapshot     = 3,  // S->C: u8 count, count * entry
    ProgressDelta= 4,  // C->S: shared inventory/world delta (or full seed). See below.
    ProgressState= 5,  // S->C: u32 version + full canonical save region (0x8F0 bytes)
    Warp         = 6,  // S->C: cross-scene /load warp (Phase 3)
};

// --- Shared inventory/world progress sync -----------------------------------
// We continuously sync a WHITELIST of the live save block (dComIfGs_getSaveData())
// so both Links share inventory + world progress, but NOT per-player stuff. The
// excluded head of the block (dSv_player_status_a/b @0x00..0x40) holds health,
// lantern oil, magic and the wolf/human transform — those stay PER-PLAYER during
// play (health is only forced on /load, Phase 3). Rupees live at 0x04 and ARE
// shared, but as an ADDITIVE counter (see below) so two simultaneous pickups both
// count instead of one overwriting the other.
constexpr uint16_t kSaveRegionSize = 0x8F0;  // mPlayer + mSave[32] + mSave2[64] + mEvent
constexpr uint16_t kRupeeOff       = 0x04;   // dSv_player_status_a_c::mRupee (BE u16)
struct SyncRange { uint16_t off, len; };
// Byte ranges merged via per-byte last-writer-wins (different chests/items touch
// different offsets, so concurrent pickups don't collide):
constexpr SyncRange kSyncRanges[] = {
    {0x09C, uint16_t(0x110 - 0x09C)},  // items / getItem / record / max / collect
    {0x1F0, uint16_t(0x8F0 - 0x1F0)},  // mSave[32] + mSave2[64] + mEvent (world + story)
};
constexpr int kMaxDeltaSpans = 64;   // cap; if exceeded we send one full snapshot

// Connection state.
socket_t   sock           = INVALID_SOCKET;
bool       enabled        = false;   // user wants to be connected
bool       connectedFlag  = false;   // TCP established + handshake sent
uint8_t    localId        = 0;
char       localName[kMaxNameLen + 1] = "Player";
char       storedHost[64] = "127.0.0.1";
int        storedPort     = 10020;
uint32_t   localSceneHash = 0;
uint32_t   reconnectCtr   = 0;

bool       connectedEvent    = false;
bool       disconnectedEvent = false;

std::vector<uint8_t> recvBuf;

PlayerState remotes[kMaxRemotePlayers];
int         remoteCount = 0;

// Per-remote-id snapshot of the kFlagInCutscene bit from the previous frame.
// Used to detect the rising edge (a peer just started a cutscene) so we can
// teleport the local player to that activator. Indexed by net player id.
bool        prevInCutscene[256] = {};

// --- Shared-Epona slave state (smoothed transform of the remote driver's horse) ---
uint8_t  horseSlaveId    = 0;       // remote id we're currently mirroring (0 = none)
cXyz     horseSmoothPos;
int16_t  horseSmoothAngle = 0;
bool     horseSmoothValid = false;
bool     localDriver     = false;   // local player is riding (driver or passenger, engine ride)
bool     localOnHorse    = false;   // local player on the horse
bool     localHasHorse   = false;   // local Epona is active this frame

// --- Shared-progress sync state ---
uint8_t  progressBaseline[kSaveRegionSize] = {};  // last save bytes we agreed with server
bool     sentInitialFull = false;  // we've pushed our full snapshot once after connect
bool     progressInSync  = false;  // we've adopted a server canonical -> may send deltas
uint32_t lastProgressVer = 0;      // highest canonical version applied
int      rupeeBaseline    = 0;     // rupee value matching progressBaseline (additive merge)

// --- little-endian writers ---
void putU8(std::vector<uint8_t>& b, uint8_t v) { b.push_back(v); }
void putU16(std::vector<uint8_t>& b, uint16_t v) {
    b.push_back(uint8_t(v)); b.push_back(uint8_t(v >> 8));
}
void putS16(std::vector<uint8_t>& b, int16_t v) { putU16(b, uint16_t(v)); }
void putU32(std::vector<uint8_t>& b, uint32_t v) {
    b.push_back(uint8_t(v)); b.push_back(uint8_t(v >> 8));
    b.push_back(uint8_t(v >> 16)); b.push_back(uint8_t(v >> 24));
}
void putF32(std::vector<uint8_t>& b, float v) {
    uint32_t u; std::memcpy(&u, &v, 4); putU32(b, u);
}

// --- little-endian readers (advance p, guarded by caller against end) ---
uint8_t  rdU8(const uint8_t*& p)  { return *p++; }
uint16_t rdU16(const uint8_t*& p) { uint16_t v = uint16_t(p[0]) | uint16_t(p[1]) << 8; p += 2; return v; }
int16_t  rdS16(const uint8_t*& p) { return int16_t(rdU16(p)); }
uint32_t rdU32(const uint8_t*& p) {
    uint32_t v = uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
    p += 4; return v;
}
float    rdF32(const uint8_t*& p) { uint32_t u = rdU32(p); float v; std::memcpy(&v, &u, 4); return v; }

void sendFramed(const std::vector<uint8_t>& payload) {
    if (sock == INVALID_SOCKET) return;
    std::vector<uint8_t> frame;
    frame.reserve(payload.size() + 2);
    putU16(frame, uint16_t(payload.size()));
    frame.insert(frame.end(), payload.begin(), payload.end());
    if (send(sock, reinterpret_cast<const char*>(frame.data()), int(frame.size()), kSendFlags) >= 0) {
        return;
    }
#if _WIN32
    const int err = WSAGetLastError();
    if (err == WSAEWOULDBLOCK || err == WSAENOTCONN) return;
#else
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == ENOTCONN) return;
#endif
    // hard error: tear down; onGameFrame() will reconnect while enabled.
    if (connectedFlag) disconnectedEvent = true;
    closeSocket(sock);
    sock = INVALID_SOCKET;
    connectedFlag = false;
    remoteCount = 0;
    reconnectCtr = 0;
}

void teardown(bool fireEvent) {
    if (sock != INVALID_SOCKET) {
        closeSocket(sock);
        sock = INVALID_SOCKET;
    }
    if (fireEvent && connectedFlag) disconnectedEvent = true;
    connectedFlag = false;
    remoteCount = 0;
    localId = 0;
    recvBuf.clear();
    // Re-handshake the shared progress on the next connection.
    sentInitialFull = false;
    progressInSync  = false;
    lastProgressVer = 0;
}

// --- shared progress helpers ---
// Pointer to the live save block (dSv_save_c). The sync whitelist offsets index
// into this. Null before a save is loaded.
uint8_t* saveBytes() { return reinterpret_cast<uint8_t*>(dComIfGs_getSaveData()); }

// Save multi-byte fields are stored big-endian (BE(u16) in d_save.h); the rupee
// counter at kRupeeOff is the one field we merge additively.
uint16_t beU16(const uint8_t* b) { return uint16_t(uint16_t(b[0]) << 8 | b[1]); }

// True when it's safe to read/write the live save for sync (in-world, not
// streaming a scene/cutscene which makes the heap/save volatile).
bool progressReady() {
    return connectedFlag && fopOvlpM_IsDoingReq() == 0 &&
           daPy_getLinkPlayerActorClass() != nullptr && saveBytes() != nullptr;
}

// Push our full whitelisted region as a seed (full=1). The relay adopts it as the
// canonical world only if it has none yet (first/main player); otherwise it's
// ignored and we adopt the relay's canonical instead.
void sendProgressFull(uint8_t* save) {
    std::vector<uint8_t> msg;
    putU8(msg, uint8_t(Msg::ProgressDelta));
    putU8(msg, 1);                                // full
    putU32(msg, lastProgressVer);                 // baseVersion
    putU32(msg, uint32_t(int32_t(dComIfGs_getRupee())));  // rupee ABSOLUTE (seed)
    putU8(msg, uint8_t(sizeof(kSyncRanges) / sizeof(kSyncRanges[0])));
    for (const SyncRange& r : kSyncRanges) {
        putU16(msg, r.off);
        putU16(msg, r.len);
        for (uint16_t i = 0; i < r.len; ++i) putU8(msg, save[r.off + i]);
        std::memcpy(progressBaseline + r.off, save + r.off, r.len);
    }
    rupeeBaseline = dComIfGs_getRupee();
    sendFramed(msg);
}

// Diff the live save against our baseline and send only what changed: rupees as a
// signed additive delta, everything else as changed byte-spans (per-byte LWW). If
// too many spans changed (e.g. just after a load) fall back to a full snapshot.
void sendProgressDelta(uint8_t* save) {
    struct Span { uint16_t off, len; };
    Span spans[kMaxDeltaSpans];
    int  nspans = 0;
    bool overflow = false;
    for (const SyncRange& r : kSyncRanges) {
        uint16_t i = 0;
        while (i < r.len) {
            if (save[r.off + i] != progressBaseline[r.off + i]) {
                const uint16_t start = i;
                while (i < r.len && save[r.off + i] != progressBaseline[r.off + i]) ++i;
                if (nspans >= kMaxDeltaSpans) { overflow = true; break; }
                spans[nspans++] = { uint16_t(r.off + start), uint16_t(i - start) };
            } else {
                ++i;
            }
        }
        if (overflow) break;
    }
    if (overflow) { sendProgressFull(save); return; }
    const int rupeeDelta = dComIfGs_getRupee() - rupeeBaseline;
    if (nspans == 0 && rupeeDelta == 0) return;  // nothing changed

    std::vector<uint8_t> msg;
    putU8(msg, uint8_t(Msg::ProgressDelta));
    putU8(msg, 0);                       // incremental
    putU32(msg, lastProgressVer);
    putU32(msg, uint32_t(int32_t(rupeeDelta)));
    putU8(msg, uint8_t(nspans));
    for (int s = 0; s < nspans; ++s) {
        putU16(msg, spans[s].off);
        putU16(msg, spans[s].len);
        for (uint16_t i = 0; i < spans[s].len; ++i) putU8(msg, save[spans[s].off + i]);
        std::memcpy(progressBaseline + spans[s].off, save + spans[s].off, spans[s].len);
    }
    rupeeBaseline = dComIfGs_getRupee();
    sendFramed(msg);
}

// Adopt a canonical world snapshot from the relay: copy the whitelisted ranges into
// the live save and reset our baseline so we don't echo them straight back.
void applyProgressState(const uint8_t* p, const uint8_t* end) {
    if (end - p < 4) return;
    const uint32_t version = rdU32(p);
    if (end - p < kSaveRegionSize) return;
    const uint8_t* region = p;
    if (progressInSync && version <= lastProgressVer) return;  // not newer
    if (!progressReady()) return;
    uint8_t* save = saveBytes();
    for (const SyncRange& r : kSyncRanges) {
        std::memcpy(save + r.off, region + r.off, r.len);
        std::memcpy(progressBaseline + r.off, region + r.off, r.len);
    }
    const uint16_t rup = beU16(region + kRupeeOff);
    dComIfGs_setRupee(rup);
    rupeeBaseline   = rup;
    lastProgressVer = version;
    progressInSync  = true;
}

// Drive the shared progress each frame: push our seed once, then send deltas.
void syncProgress() {
    if (!progressReady()) return;
    uint8_t* save = saveBytes();
    if (!sentInitialFull) {
        sendProgressFull(save);
        sentInitialFull = true;  // wait to adopt the relay canonical before sending deltas
        return;
    }
    if (progressInSync) sendProgressDelta(save);
}

void handleMessage(const uint8_t* p, size_t len) {
    if (len < 1) return;
    const uint8_t* end = p + len;
    Msg type = Msg(rdU8(p));
    switch (type) {
        case Msg::Welcome:
            if (p < end) {
                localId = rdU8(p);
                if (!connectedFlag) {
                    connectedFlag = true;
                    connectedEvent = true;
                }
                DuskLog.info("net: joined as player {}", localId);
            }
            break;
        case Msg::Snapshot: {
            if (p >= end) break;
            int count = rdU8(p);
            remoteCount = 0;
            for (int i = 0; i < count && remoteCount < kMaxRemotePlayers; ++i) {
                if (end - p < 2) break;
                PlayerState s{};
                s.id = rdU8(p);
                int nlen = rdU8(p);
                int copy = nlen > kMaxNameLen ? kMaxNameLen : nlen;
                if (end - p < nlen) break;
                std::memcpy(s.name, p, copy);
                s.name[copy] = '\0';
                p += nlen;
                // fixed tail: u32 scene + s8 room + 3f + s16 + u16 anim + u16
                //   animUpper + u8 costume + u8 flags + 24 eventName + 8 stageName
                //   + 3f horse + s16 horseAngle + u16 horseAnim + u8 seat
                //   = 4+1+12+2+2+2+1+1+24+8 + 12+2+2+1 = 74
                if (end - p < 74) break;
                s.sceneHash = rdU32(p);
                s.room      = int8_t(rdU8(p));
                s.posX = rdF32(p); s.posY = rdF32(p); s.posZ = rdF32(p);
                s.angleY    = rdS16(p);
                s.anim      = rdU16(p);
                s.animUpper = rdU16(p);
                s.costume   = rdU8(p);
                s.flags     = rdU8(p);
                for (int k = 0; k < kMaxEventName + 1; ++k) s.eventName[k] = char(rdU8(p));
                s.eventName[kMaxEventName] = '\0';
                for (int k = 0; k < kStageNameLen; ++k) s.stageName[k] = char(rdU8(p));
                s.horseX = rdF32(p); s.horseY = rdF32(p); s.horseZ = rdF32(p);
                s.horseAngleY = rdS16(p);
                s.horseAnim = rdU16(p);
                s.seat = rdU8(p);
                if (s.id == localId) continue;  // ignore our own echo
                remotes[remoteCount++] = s;
            }
            break;
        }
        case Msg::ProgressState:
            applyProgressState(p, end);
            break;
        default:
            break;
    }
}

void pump() {
    if (sock == INVALID_SOCKET) return;
    char tmp[4096];
    for (;;) {
        const int r = recv(sock, tmp, sizeof(tmp), 0);
        if (r > 0) { recvBuf.insert(recvBuf.end(), tmp, tmp + r); continue; }
        if (r == 0) { teardown(true); return; }
#if _WIN32
        if (WSAGetLastError() == WSAEWOULDBLOCK) break;
#else
        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
#endif
        teardown(true);
        return;
    }
    size_t off = 0;
    while (recvBuf.size() - off >= 2) {
        uint16_t len = uint16_t(recvBuf[off]) | uint16_t(recvBuf[off + 1]) << 8;
        if (recvBuf.size() - off - 2 < len) break;
        handleMessage(recvBuf.data() + off + 2, len);
        off += 2 + len;
    }
    if (off > 0) recvBuf.erase(recvBuf.begin(), recvBuf.begin() + off);
}

// Open a non-blocking socket and start connecting. Handshake (Hello) is queued
// immediately; Welcome from the server flips connectedFlag.
void openConnection() {
    teardown(false);

    sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) return;
#if _WIN32
    u_long nb = 1;
    if (ioctlsocket(sock, FIONBIO, &nb) != 0) { teardown(false); return; }
#else
    const int fl = fcntl(sock, F_GETFL, 0);
    if (fl < 0 || fcntl(sock, F_SETFL, fl | O_NONBLOCK) < 0) { teardown(false); return; }
#endif
#if defined(__APPLE__)
    { int opt = 1; setsockopt(sock, SOL_SOCKET, SO_NOSIGPIPE, &opt, sizeof(opt)); }
#endif
    { int one = 1; setsockopt(sock, IPPROTO_TCP, TCP_NODELAY,
                              reinterpret_cast<const char*>(&one), sizeof(one)); }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(uint16_t(storedPort));
    if (inet_pton(AF_INET, storedHost, &addr.sin_addr) != 1) { teardown(false); return; }

    const int cr = ::connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
#if _WIN32
    const bool pending = cr < 0 && WSAGetLastError() == WSAEWOULDBLOCK;
#else
    const bool pending = cr < 0 && errno == EINPROGRESS;
#endif
    if (cr != 0 && !pending) { teardown(false); return; }

    recvBuf.clear();

    // Queue the handshake (will flush once the socket is writable).
    std::vector<uint8_t> hello;
    putU8(hello, uint8_t(Msg::Hello));
    putU8(hello, kProtocolVersion);
    const uint8_t nlen = uint8_t(std::strlen(localName));
    putU8(hello, nlen);
    for (uint8_t i = 0; i < nlen; ++i) putU8(hello, uint8_t(localName[i]));
    sendFramed(hello);

    DuskLog.info("net: connecting to {}:{} as {}", storedHost, storedPort, localName);
}

bool readLocalState(PlayerState& out) {
    daPy_py_c* link = daPy_getLinkPlayerActorClass();
    if (link == nullptr) return false;
    const char* stage = dComIfGp_getStartStageName();
    localSceneHash = hashScene(stage);
    out.id        = localId;
    out.sceneHash = localSceneHash;
    std::memset(out.stageName, 0, sizeof(out.stageName));
    if (stage != nullptr) std::strncpy(out.stageName, stage, sizeof(out.stageName) - 1);
    out.room      = int8_t(dComIfGp_roomControl_getStayNo());
    out.posX      = link->current.pos.x;
    out.posY      = link->current.pos.y;
    out.posZ      = link->current.pos.z;
    out.angleY    = link->shape_angle.y;
    daAlink_c* al = static_cast<daAlink_c*>(link);
    out.anim      = al->getDuskBodyAnmIdx();   // legs/locomotion
    out.animUpper = al->getDuskUpperAnmIdx();  // arms/torso
    out.costume   = dComIfGs_getSelectEquipClothes();  // dItemNo_WEAR_* -> puppet model
    out.flags     = 0;
    out.horseX = out.horseY = out.horseZ = 0.0f;
    out.horseAngleY = 0;
    out.horseAnim = 0;
    out.seat = 0;
    // Shared-Epona: publish the horse transform + anim whenever our Epona is ACTIVE
    // (present, not parked far away), not only while riding — so peers see and slave
    // to it even before anyone mounts. kFlagOnHorse additionally marks that we're
    // the one riding it.
    daHorse_c* horse = dComIfGp_getHorseActor();
    if (horse != nullptr && horse->duskActive()) {
        out.flags |= kFlagHasHorse;
        out.horseX = horse->current.pos.x;
        out.horseY = horse->current.pos.y;
        out.horseZ = horse->current.pos.z;
        out.horseAngleY = horse->shape_angle.y;
        out.horseAnim = horse->getAnmIdx(0);
        if (al->checkHorseRide()) out.flags |= (kFlagOnHorse | kFlagHorseDriver);
    }
    localDriver   = (out.flags & kFlagHorseDriver) != 0;
    localOnHorse  = (out.flags & kFlagOnHorse) != 0;
    localHasHorse = (out.flags & kFlagHasHorse) != 0;
    out.eventName[0] = '\0';
    if (al->getDemoMode() != 0 || dComIfGp_event_runCheck() != 0) {
        out.flags |= kFlagInCutscene;  // remotes hide this puppet during cutscenes/demos
        // Publish the running cutscene name so peers can play the same one locally.
        const char* ev = dComIfGp_getPEvtManager()->getRunEventName();
        if (ev != nullptr) std::snprintf(out.eventName, sizeof(out.eventName), "%s", ev);
    }
    return true;
}

}  // namespace

uint32_t hashScene(const char* name) {
    // FNV-1a 32-bit.
    uint32_t h = 2166136261u;
    if (name) {
        for (const char* c = name; *c; ++c) {
            h ^= uint8_t(*c);
            h *= 16777619u;
        }
    }
    return h;
}

void connect(const char* host, int port, const char* name) {
#if _WIN32
    WSADATA wd{};
    WSAStartup(MAKEWORD(2, 2), &wd);
#endif
    if (host && *host) std::snprintf(storedHost, sizeof(storedHost), "%s", host);
    if (port > 0) storedPort = port;
    if (name && *name) std::snprintf(localName, sizeof(localName), "%s", name);

    enabled = true;
    reconnectCtr = 0;
    openConnection();
}

void disconnect() {
    enabled = false;
    teardown(true);
}

bool isConnected() { return connectedFlag && sock != INVALID_SOCKET; }
bool isEnabled()   { return enabled; }

uint8_t     getLocalId()        { return localId; }
const char* getLocalName()      { return localName; }
uint32_t    getLocalSceneHash() { return localSceneHash; }

int getRemotePlayerCount() { return remoteCount; }
const PlayerState* getRemotePlayer(int idx) {
    if (idx < 0 || idx >= remoteCount) return nullptr;
    return &remotes[idx];
}

const PlayerState* getRemotePlayerById(uint8_t id) {
    for (int i = 0; i < remoteCount; ++i) {
        if (remotes[i].id == id) return &remotes[i];
    }
    return nullptr;
}

uint8_t getHorseDriverId() {
    // The DRIVER actually controls the horse (kFlagHorseDriver). Passengers carry
    // kFlagOnHorse but not kFlagHorseDriver, so they never become the driver.
    uint8_t best = 0;
    bool have = false;
    if (localDriver && localId != 0) { best = localId; have = true; }
    for (int i = 0; i < remoteCount; ++i) {
        const PlayerState& s = remotes[i];
        if ((s.flags & kFlagHorseDriver) == 0 || s.sceneHash != localSceneHash) continue;
        if (!have || s.id < best) { best = s.id; have = true; }
    }
    return have ? best : 0;
}

bool isHorseDriver(uint8_t id) { return id != 0 && id == getHorseDriverId(); }

// Owner of the shared horse: the driver if anyone is riding, otherwise the lowest
// id that simply HAS an active Epona out. Everyone else slaves their Epona to it.
uint8_t getHorseOwnerId() {
    const uint8_t drv = getHorseDriverId();
    if (drv != 0) return drv;
    uint8_t best = 0;
    bool have = false;
    if (localHasHorse && localId != 0) { best = localId; have = true; }
    for (int i = 0; i < remoteCount; ++i) {
        const PlayerState& s = remotes[i];
        if ((s.flags & kFlagHasHorse) == 0 || s.sceneHash != localSceneHash) continue;
        if (!have || s.id < best) { best = s.id; have = true; }
    }
    return have ? best : 0;
}

bool hasRemoteHorseOwner() {
    const uint8_t o = getHorseOwnerId();
    return o != 0 && o != localId;
}

bool getRemoteHorse(float* x, float* y, float* z, int16_t* angleY, uint16_t* anim) {
    // Slave to the shared horse's OWNER (driver if mounted, else lowest horse
    // holder). If the owner is us, or nobody has a horse, there's nothing to mirror.
    const uint8_t owner = getHorseOwnerId();
    const PlayerState* s = (owner != 0 && owner != localId) ? getRemotePlayerById(owner) : nullptr;
    if (s != nullptr && (s->flags & kFlagHasHorse) != 0 && s->sceneHash == localSceneHash) {
        const float dx = s->horseX - horseSmoothPos.x;
        const float dy = s->horseY - horseSmoothPos.y;
        const float dz = s->horseZ - horseSmoothPos.z;
        const float d2 = dx * dx + dy * dy + dz * dz;
        if (!horseSmoothValid || horseSlaveId != s->id || d2 > 300.0f * 300.0f) {
            horseSmoothPos.set(s->horseX, s->horseY, s->horseZ);  // snap (new driver / jump)
            horseSmoothAngle = s->horseAngleY;
        } else {
            const float k = 0.5f;
            horseSmoothPos.x += dx * k;
            horseSmoothPos.y += dy * k;
            horseSmoothPos.z += dz * k;
            horseSmoothAngle += (int16_t)((int16_t)(s->horseAngleY - horseSmoothAngle) * k);
        }
        horseSlaveId     = s->id;
        horseSmoothValid = true;
        if (x) *x = horseSmoothPos.x;
        if (y) *y = horseSmoothPos.y;
        if (z) *z = horseSmoothPos.z;
        if (angleY) *angleY = horseSmoothAngle;
        if (anim) *anim = s->horseAnim;
        return true;
    }
    horseSmoothValid = false;
    horseSlaveId = 0;
    return false;
}

bool consumeConnectedEvent()    { bool v = connectedEvent;    connectedEvent    = false; return v; }
bool consumeDisconnectedEvent() { bool v = disconnectedEvent; disconnectedEvent = false; return v; }

// Co-op cutscene follow: when a remote peer starts a cutscene, yank the local
// player over to where that peer triggered it so everyone watches together.
// We act only on the RISING edge of the peer's in-cutscene flag (the frame they
// enter it), in the same scene, and never while the local player is themselves
// in a cutscene/demo (don't fight the local event/camera).
void processCutsceneTeleport() {
    daPy_py_c* link = daPy_getLinkPlayerActorClass();
    if (link == nullptr) return;
    daAlink_c* al = static_cast<daAlink_c*>(link);
    const bool localBusy = al->getDemoMode() != 0 || dComIfGp_event_runCheck() != 0;

    for (int i = 0; i < remoteCount; ++i) {
        const PlayerState& s = remotes[i];
        const bool nowCs = (s.flags & kFlagInCutscene) != 0;
        const bool wasCs = prevInCutscene[s.id];
        prevInCutscene[s.id] = nowCs;
        if (!nowCs || wasCs) continue;                // rising edge only
        if (s.sceneHash != localSceneHash) continue;  // must share our scene
        if (localBusy) continue;                      // don't interrupt our own cutscene

        cXyz pos(s.posX, s.posY, s.posZ);
        al->setPlayerPosAndAngle(&pos, s.angleY, TRUE);
        DuskLog.info("net: cutscene by player {} -> teleport to {} {} {}",
                     (int)s.id, s.posX, s.posY, s.posZ);
    }
}

// Co-op passenger: the engine's Epona carries one rider, so a non-driver can't
// mount the shared (slaved) horse normally. When standing next to a remote-owned
// shared horse and pressing A, force the local Link straight into the real
// riding-wait state on it (proper seated pose; the engine keeps Link on the saddle
// and handles dismount). The slaved horse ignores this rider's steering, so the
// passenger just rides along. They appear behind the driver via the puppet rear
// seat (a rider that isn't the lowest kFlagHorseDriver is a passenger).
void processPassenger() {
    daPy_py_c* link = daPy_getLinkPlayerActorClass();
    if (link == nullptr) return;
    daAlink_c* al = static_cast<daAlink_c*>(link);
    daHorse_c* horse = dComIfGp_getHorseActor();
    if (horse == nullptr || !horse->duskActive() || !hasRemoteHorseOwner()) return;
    if (al->checkHorseRide()) return;  // already a passenger; setSyncHorsePos seats us
    // On foot next to the shared horse: press A to board as a passenger. Once riding,
    // the engine (setSyncHorsePos) keeps us at the rear seat and handles dismount.
    if (al->getDemoMode() != 0 || dComIfGp_event_runCheck() != 0) return;
    if (mDoCPd_c::getTrigA(PAD_1) == 0) return;
    const f32 dx = horse->current.pos.x - link->current.pos.x;
    const f32 dz = horse->current.pos.z - link->current.pos.z;
    if (dx * dx + dz * dz < 250.0f * 250.0f) {
        al->duskBoardHorsePassenger();
        DuskLog.info("net: boarded shared horse as passenger");
    }
}

// Set by the main-menu "Multiplayer" button: connect to the stored endpoint
// once a game is actually running (the player actor exists).
bool autoConnectPending = false;

void onGameFrame() {
    if (autoConnectPending) {
        if (daPy_getLinkPlayerActorClass() == nullptr) {
            return;  // game not in-world yet; wait
        }
        autoConnectPending = false;
        connect(nullptr, 0, nullptr);  // uses stored host/port/name
    }
    if (!enabled) return;

    if (sock == INVALID_SOCKET) {
        // Lazy reconnect roughly once per second (assuming ~30-60 fps).
        if ((reconnectCtr++ % 45) == 0) openConnection();
        return;
    }

    pump();
    if (sock == INVALID_SOCKET) return;  // pump() may have torn down

    PlayerState local{};
    if (readLocalState(local)) {
        std::vector<uint8_t> msg;
        putU8(msg, uint8_t(Msg::State));
        putU32(msg, local.sceneHash);
        putU8(msg, uint8_t(local.room));
        putF32(msg, local.posX); putF32(msg, local.posY); putF32(msg, local.posZ);
        putS16(msg, local.angleY);
        putU16(msg, local.anim);
        putU16(msg, local.animUpper);
        putU8(msg, local.costume);
        putU8(msg, local.flags);
        for (int i = 0; i < kMaxEventName + 1; ++i) putU8(msg, uint8_t(local.eventName[i]));
        for (int i = 0; i < kStageNameLen; ++i) putU8(msg, uint8_t(local.stageName[i]));
        putF32(msg, local.horseX); putF32(msg, local.horseY); putF32(msg, local.horseZ);
        putS16(msg, local.horseAngleY);
        putU16(msg, local.horseAnim);
        putU8(msg, local.seat);
        sendFramed(msg);
    }

    // Follow a peer into the cutscene they just triggered (teleport to activator).
    processCutsceneTeleport();

    // Board/stay on a remote's shared horse as a passenger (rear seat).
    processPassenger();

    // Push/adopt the shared inventory + world progress.
    syncProgress();
}

void requestAutoConnect() {
    autoConnectPending = true;
}

void setEndpoint(const char* host, int port, const char* name) {
    if (host && *host) std::snprintf(storedHost, sizeof(storedHost), "%s", host);
    if (port > 0) storedPort = port;
    if (name && *name) std::snprintf(localName, sizeof(localName), "%s", name);
}

void shutdown() {
    disconnect();
#if _WIN32
    WSACleanup();
#endif
}

}  // namespace dusk::net
