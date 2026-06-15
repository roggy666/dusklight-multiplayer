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
#include "d/d_com_inf_game.h"     // dComIfGp_roomControl_getStayNo(), getStartStageName()

#include <cstdio>
#include <cstring>
#include <vector>

namespace dusk::net {
namespace {

enum class Msg : uint8_t {
    Hello    = 0,  // C->S: u8 version, u8 nameLen, name
    Welcome  = 1,  // S->C: u8 id
    State    = 2,  // C->S: u32 scene, s8 room, 3f pos, s16 angleY, u16 anim
    Snapshot = 3,  // S->C: u8 count, count * entry
};

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
                //   animUpper + u8 costume + u8 flags + 24 eventName
                //   = 4+1+12+2+2+2+1+1+24 = 49
                if (end - p < 49) break;
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
                if (s.id == localId) continue;  // ignore our own echo
                remotes[remoteCount++] = s;
            }
            break;
        }
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

bool consumeConnectedEvent()    { bool v = connectedEvent;    connectedEvent    = false; return v; }
bool consumeDisconnectedEvent() { bool v = disconnectedEvent; disconnectedEvent = false; return v; }

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
        sendFramed(msg);
    }
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
