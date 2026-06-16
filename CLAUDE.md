# Dusklight — Multiplayer (puppet) work context

This repo is a Twilight Princess PC decomp (TwilitRealm/dusklight). Active work =
an **uncommitted networked co-op multiplayer** feature (all in the working tree,
nothing committed). This file is the handoff context for continuing it.

## Build & run (Windows, MSVC)
- Build: `.\dusk_build.bat build-target dusklight` (PowerShell, from repo root).
  - exe: `build/windows-msvc-relwithdebinfo/dusklight.exe`, preset
    `windows-msvc-relwithdebinfo`. `vswhere not recognized` warning is harmless.
  - The exe is **locked while the game runs** → kill `dusklight` procs before linking.
- Relay server (Python, dependency-free): `tools/mp_server/relay_server.py`
  `--host 0.0.0.0 --port 10020 --tick 60`. **Restart it whenever the wire protocol
  changes** (its `_STATE_FMT` must match `net.cpp`).
- Test loop: kill `dusklight`, build, (restart relay if protocol changed), launch
  TWO `dusklight.exe`, then in each: load a save / reach gameplay, press the
  **Multiplayer** menu item (auto-connects to 127.0.0.1:10020), bring both Links
  into the same area to see each other's puppet.
- Logs: `%APPDATA%\TwilitRealm\Dusklight\logs\dusklight-*.log` (the two newest =
  the two clients). Crash reports land here too (see crash handler).
- Symbolicate a crash: `python tools/resolve_crash.py <logfile>` (uses
  `build/.../dusklight.map`, preferred base 0x140000000).

## Architecture
- **Transport** `src/dusk/net.cpp` + `include/dusk/net.h` — non-blocking TCP client,
  framed `[u16 LE len][payload]`, FNV-1a scene hash. `onGameFrame()` (called from
  `f_ap_game.cpp` `duskExecute`) reconnects, reads local Link state, sends it, polls
  remotes. `PlayerState` = the synced per-player struct.
- **Puppet** `src/d/actor/d_a_dusk_puppet.cpp` + header — render-only actor
  (`fpcNm_DUSK_PUPPET_e` = 0x318) drawing a Link body at each remote's transform.
  Manager `daDuskPuppet_updateAll()` (called from `f_ap_game.cpp`) spawns/despawns
  one puppet per remote. NOT the player singleton.
- **UI/nameplates** `src/dusk/imgui/ImGuiMultiplayer.cpp` (connect window +
  `ShowMultiplayerNameplates`, gated by `daDuskPuppet_remotesVisible()`).
- **Relay** `tools/mp_server/relay_server.py` — keeps latest state per client,
  broadcasts snapshots at tick rate. Opaque passthrough of `_STATE_FMT`.
- Vanilla hooks: `f_ap_game.cpp` (per-frame calls + `crash_handler::heartbeat`),
  `d_a_alink.cpp` (3 null-guards in `*ModelCallBack` for the shared body model),
  `d_a_alink.h` (added `getDuskBodyAnmIdx`/`getDuskUpperAnmIdx`),
  `f_pc_name.h`/`f_pc_profile_lst.{h,cpp}`/`files.cmake` (puppet profile reg).

## Wire protocol (v5) — keep net.h `kProtocolVersion`, net.cpp, relay `_STATE_FMT` in sync
State msg (C->S) tail after type byte, all little-endian:
`u32 sceneHash, s8 room, f32 x,y,z, s16 angleY, u16 anim(legs/UNDER_0),`
`u16 animUpper(arms/UPPER_2), u8 costume(dItemNo_WEAR_*), u8 flags, char[24] eventName`.
`flags` bit0 = `kFlagInCutscene`. relay `_STATE_FMT="<IbfffhHHBB24s"`.

## What WORKS (done, tested)
1. **No crashes.** The whole reason puppets are stable:
   - Title/menu attract demo + in-game cutscenes ran the play scene with a live
     Link, so puppets used to spawn there and hang the GX FIFO (`write_data_grow`
     realloc → ~2GB). Gate: `daDuskPuppet_remotesVisible()` returns false during
     `getDemoMode()!=0` or `dComIfGp_event_runCheck()!=0`.
   - **Cutscene crash (cross-client):** a remote in a cutscene broadcasts
     **demo-archive anim ids** that are garbage in our standard anm archive →
     parsing them made a bad BCK → null deref in the puppet skeleton calc. Fix:
     sync `kFlagInCutscene`; the manager despawns a remote's puppet while that
     remote is in a cutscene, AND `Execute`/`Draw` self-gate on
     `fopOvlpM_IsDoingReq()` + `remotesVisible()` + the remote's flag (actor
     Execute runs in `fpcM_Management` BEFORE the manager, so the manager gate
     alone is too late — self-gate is required).
   - Scene/cutscene transitions reset the archive heap → cached costume models +
     anim BCK copies dangle. Manager clears ALL puppet caches when
     `fopOvlpM_IsDoingReq()!=0` (streaming) and respawns after.
2. **Costume sync** — protocol carries `costume` (`dComIfGs_getSelectEquipClothes()`);
   puppet loads the matching archive `Bmdl`(casual/Ordon)/`Kmdl`(hero)/`Zmdl`(zora)/
   `Mmdl`(magic). Model filenames differ per archive: derive prefix from the
   `*_head.bmd` file → body `<prefix>.bmd`, hands `<prefix>_hands.bmd`. **Face is the
   shared model `al_face.bmd`** in every archive (NOT `<prefix>_face`).
3. **Face** — `al_face.bmd` loaded, attached to body joint 4, default face
   texture-pattern `BTP_FA` baked into its shared DL (`bakeFaceTextures`: the models
   use a baked single DL from `loaderBasicBmd`→`makeSharedDL`, so a runtime BTP
   can't change texNo — must re-bake). Hands: NOT drawn separately (body model
   already has hands; the separate `*_hands` model duplicated them).
4. **Animations** — body BCKs synced as resource ids and loaded from
   `dComIfGp_getAnmArchive()` as PRIVATE decompressed copies (JKRReadIdxResource +
   `mDoExt_transAnmBas` + `J3DAnmLoaderDataBase::setResource`; **never** parse the
   live archive resource — it corrupts Link's own anims). Cache `getAnimById`
   (64 entries, invalidated when the anm archive ptr changes). Upper/under split:
   custom `PuppetBodyMtxCalc` (a `J3DMtxCalcNoAnm<...Maya...>`) set as the body
   model's joint-0 MtxCalc — joints 2..0x11 use the UPPER (arms) anim, the rest use
   UNDER (legs). Per-puppet frame counters; reset on id change. (Arm joints: chains
   7-9 and 0xC-0xE; head 4; legs from 0x12 — from `armJointTable`/`footJointTable`
   in d_a_alink.cpp.)
5. **Smoothing** — puppet position/angle exponentially interpolated (k=0.35, snap
   if jump >3 m); relay runs at `--tick 60`.

## OPEN / NOT done
- **Teleport-to-activator (DONE, first step).** When a remote peer starts a
  cutscene (rising edge of `kFlagInCutscene`), `net.cpp processCutsceneTeleport()`
  (called from `onGameFrame` after the send) warps the LOCAL Link to that peer's
  reported pos/angle via `daAlink_c::setPlayerPosAndAngle(&pos, angleY, TRUE)`.
  Gated: same scene (`sceneHash`), and skipped if the local player is themselves
  in a demo/event (don't fight the local camera). Reuses existing protocol fields
  — no wire/relay change. Edge state: `prevInCutscene[256]` indexed by net id.
  Still TODO below: actually PLAY the same cutscene on the receiver.
- **Co-op cutscene sync (play on receiver, UNSOLVED).** Goal: when player A
  triggers a cutscene, player B also plays it (with B's local Link). Findings so
  far: the event NAME is correctly synced
  (`getRunEventName()` → e.g. `demo01_04`, `demo38_01`; also returns
  `"NO DATA"`/`"DEFAULT_START"` when not a real event). `startCheck(name)` only
  *reports* whether an event is already running. `order(getEventIdx(name,0xFF))`
  force-starts via `startProc` — BUT in testing it did **not** actually play the
  cutscene on B, and it spuriously fired on zone-entry demos, freezing the
  receiver's camera. **This whole auto-trigger was REVERTED** (the `eventName`
  field is still in the protocol but currently unused on receive). Needs a careful
  approach: a whitelist/condition for which events to sync (exclude entry/auto
  demos), the right start API for demo-vs-event cutscenes (demos go through
  `setStartDemo(mapToolId)`, not the event manager), and time/position sync.
- **Animation crossfade** — tried a manual prev→cur Euler-lerp morph; it slowed the
  arms and was removed. Anim switches are currently instant (slightly abrupt). A
  proper morph (via the engine's real morf, not hand-rolled) is a TODO.

## Crash handler (already committed upstream, upgraded this work)
`src/dusk/crash_handler.cpp` installed from `m_Do_main.cpp`. Windows additions
(flush `_commit`, vectored handler, **hang watchdog** with all-thread dump, CRT
fatal handlers). `crash_handler::heartbeat()` pulsed once/frame from
`f_ap_game.cpp duskExecute`. This is what made every puppet bug debuggable.

## Gotchas learned
- Actor `Execute`/`Draw` run during `fpcM_Management`, BEFORE the end-of-frame
  manager hooks — so puppet self-gating must use live checks, not manager state.
- Puppet models/anims live on the **volatile archive heap**; any scene/cutscene
  stream (`fopOvlpM_IsDoingReq`) frees them → must drop caches + despawn.
- The anm-archive POINTER does NOT reliably change on a heap reset; use the
  streaming flag as the invalidation signal.
- Costume model files are YAZ0-compressed → size via `getResSize` is the compressed
  size; use `JKRReadIdxResource` (decompresses) for a full copy, not a raw memcpy.

See also memory files: `dusklight-multiplayer`, `dusklight-crash-handler`.
