# Dusklight Multiplayer (Path B — native state-sync)

A lightweight multiplayer layer built **natively into dusklight**. It is *not*
the original TPOnline mod: that mod is a PowerPC `main.dol` patch using
deterministic lockstep, which cannot run on a native reimplementation. This
instead syncs each player's transform over a small relay server and draws the
other players in-world.

## What works

- Each connected client publishes Link's position / facing / current area every
  frame.
- The relay server broadcasts everyone's state to everyone else.
- In-game, remote players in **the same area** are drawn as **world-anchored
  nameplates + markers** (name tag above the head, colored dot at the feet),
  projected through the game camera. Works in every scene — no per-scene model
  archives required.
- A **Multiplayer** window (debug menu → *Multiplayer*) to set host / port /
  name, connect/disconnect, and view the live player list with distances.

## Running

### 1. Start the relay server

Anyone (one player, or a dedicated host) runs:

```
python relay_server.py            # listens on 0.0.0.0:10020
python relay_server.py --port 10020 --tick 30
```

No dependencies — pure Python 3.8+. For internet play, forward the TCP port or
run it on a VPS / LAN host.

### 2. Connect from dusklight

1. Launch dusklight and load your game.
2. Open the debug menu and pick **Multiplayer**.
3. Enter the server **Host** (e.g. `127.0.0.1` for local, or the host's IP),
   **Port** (`10020`), and your **Name**.
4. Press **Connect**.

Have each player do the same, pointing at the same server. When two players are
in the same area you'll see each other's nameplate move in real time.

## Protocol

Framed TCP: `[u16 LE length][payload]`, `payload[0]` = type. See the docstring in
`relay_server.py` and `src/dusk/net.cpp` for the exact byte layout. Both ends
are kept in sync by hand; bump `kProtocolVersion` on any change.

## Roadmap (not yet implemented)

- **Animated body** instead of a nameplate (the big one): a custom puppet actor
  that renders a Link model and plays a shared animation id. dusklight has no
  built-in "second player" actor (Link is a singleton via
  `dComIfGp_setPlayer(0,...)`), so this needs a new actor + a model resource.
  The `anim` field is already reserved in the protocol for this.
- Persist host/port/name to the dusklight config.
- Interpolation/extrapolation between snapshots for extra smoothness.
- Voice/text chat, item/event sync.
