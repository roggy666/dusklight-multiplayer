#ifndef DUSK_NET_H
#define DUSK_NET_H

#include <cstdint>

// Dusklight multiplayer (Path B: native state-sync, NOT the original TPOnline
// deterministic lockstep). This module is the client transport + per-frame
// hook. It connects to a small relay server (tools/mp_server/relay_server.py),
// publishes the local player's transform every frame, and exposes the latest
// transforms of remote players so the overlay/avatar layer can render them.
//
// See memory: tponline-protocol / dusklight-vs-tponline-incompatibility.

namespace dusk::net {

// Wire protocol version. Bump on any change to the message layout.
constexpr uint8_t kProtocolVersion = 5;

// Max length of a synced cutscene/event name (fixed field in the state message).
constexpr int kMaxEventName = 23;

// Maximum simultaneously tracked remote players (relay caps at 16 total).
constexpr int kMaxRemotePlayers = 15;
constexpr int kMaxNameLen       = 15;

// One player's synchronized state. POD; mirrors the wire layout in net.cpp and
// relay_server.py.
struct PlayerState {
    uint8_t  id;                       // server-assigned player id
    char     name[kMaxNameLen + 1];    // null-terminated display name
    uint32_t sceneHash;                // hash of current stage name (same-area filter)
    int8_t   room;                     // dComIfGp_roomControl_getStayNo()
    float    posX, posY, posZ;         // current.pos
    int16_t  angleY;                   // shape_angle.y (facing)
    uint16_t anim;                     // lower-body BCK id (legs/locomotion; UNDER_0)
    uint16_t animUpper;                // upper-body BCK id (arms/torso; UPPER_2)
    uint8_t  costume;                  // dItemNo_WEAR_* of current clothes (puppet model)
    uint8_t  flags;                    // bit0 = in a cutscene/demo (hide this puppet)
    char     eventName[kMaxEventName + 1];  // running cutscene name (empty if none)
};

// PlayerState::flags bits.
enum { kFlagInCutscene = 1 << 0 };

// --- Lifecycle (driven from the multiplayer UI) ---
// connect() stores host/port/name and opens a non-blocking connection; the
// client auto-reconnects from onGameFrame() while enabled.
void connect(const char* host, int port, const char* name);
void disconnect();
bool isConnected();
bool isEnabled();                      // true between connect() and disconnect()

// Store the endpoint without connecting (e.g. from a menu before launch).
void setEndpoint(const char* host, int port, const char* name);
// Connect to the stored endpoint once a game is running (deferred). Used by the
// main-menu "Multiplayer" button.
void requestAutoConnect();

// Called once per game frame from f_ap_game.cpp (PC build only):
//   - (re)establishes the connection if needed,
//   - reads the local Link transform and sends it,
//   - polls the socket and refreshes the remote player table.
void onGameFrame();

// --- Remote players (read by the nameplate/avatar layer) ---
int                getRemotePlayerCount();
const PlayerState* getRemotePlayer(int idx);
const PlayerState* getRemotePlayerById(uint8_t id);  // null if not present

uint8_t     getLocalId();              // 0 until the server sends Welcome
const char* getLocalName();
uint32_t    getLocalSceneHash();       // local player's current scene hash

// Connection events for UI toasts (consume = read-and-clear).
bool consumeConnectedEvent();
bool consumeDisconnectedEvent();

// FNV-1a hash used for scene keys; exposed so the overlay can compare.
uint32_t hashScene(const char* name);

void shutdown();

}  // namespace dusk::net

#endif  // DUSK_NET_H
