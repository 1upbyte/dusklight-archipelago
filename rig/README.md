# Test rig

Scripts for developing and testing the mod and the apworld. Run them from the repo root, e.g.
`python rig/tracker_parity.py presets/Easy.yaml`. Where they find Archipelago, its Python and
their scratch space is in `paths.py` (defaults: `../ap`, `../venv`, `build/rig`; override with
`AP_DIR`, `AP_PYTHON`, `RIG_WORK`). Anything that generates seeds needs the apworld installed in
that checkout first: `python tools/build_apworld.py --install <AP_DIR>/custom_worlds`.

The `.cmd` build scripts are for Windows; on other platforms run the CMake commands in the main
README's Building section.

- `build_mod.cmd [--target X]` — build the mod (MSVC + Ninja). Output: `build/mods/archipelago.dusk`.
- `build_mod_android.cmd` — cross-build the mod for Android (arm64-v8a, API 28), same
  toolchain as CI. Output: `build-android/mods/archipelago.dusk`.
- `build_dusk2.cmd` — build Dusklight itself from the pinned checkout (`dusklight/`), for running
  the mod locally.
- `deathlink_probe.py <slot> [server] [password]` — joins a room as another slot with the
  DeathLink tag, prints deaths the game sends, sends one when you press Enter. Needs a
  two-slot seed (your YAML + one named after the probe's slot).
- `cover_probe.py [out_dir] [game ...]` — searches SteamGridDB for ~90 real Archipelago game names
  the way the mod does and prints which game the mod's matcher picks for each; decodes a few
  covers too. Needs your own key in `SGDB_KEY` or `rig/sgdb_key.txt` (git-ignored).
- `ws_proxy.py` — WebSocket proxy on :38282 that logs every message between the game and an
  Archipelago server on :38281. The quickest way to see what the game is or isn't sending.
- `dump_slot.py [slot]` — pull slot_data out of the newest generated multiworld zip.
- `tp_reach.py` — build the apworld in-process and report unreachable locations / beatability.
- `sweep_options.py` — generate a seed for every value of every option (~30 min).
- `count_checks.py presets/X.yaml ...` — checks per preset, averaged over 8 seeds (dungeon
  picks vary per seed). With no arguments, probes the floor and ceiling.
- `sweep_dungeons.py` — 80 seeds across Shuffled Dungeons x key modes x rewards x scarcity,
  each rebuilt with `ap_gen_test` and required to match exactly. `sweep_dungeons.py
  presets/*.yaml` does the same for every preset over 4 seeds (the release check).
- `tracker_parity.py presets/X.yaml [seed ...] [--players N]` — the in-game tracker's logic
  (src/ap/ap_tracker.cpp via `ap_gen_test --tracker`) against the apworld's own rules: replays the
  playthrough sphere by sphere and compares "in logic" at seven points. With `--players 2`, items
  from the other world arrive as received items, as the server sends them.
- `check_exclusion.py presets/X.yaml ...` — reads item classification flags from the
  multidata to confirm Hyrule Castle holds nothing important with the explicit line, plus a
  control without it (the default excludes nothing since 0.6.0).
- `debug_prefill.py <preset> <seed> [--no-held-keys]` — when a fill fails: reachability with
  every item, which progression survived, what got pruned, and which single item unblocks
  the most.
- `deflate_handshake_check.py` — sends the mod's exact upgrade request to a server configured
  like MultiServer and runs MultiServer's own compression check on it.
- `build_mod.cmd --target tls_test` then `build/tls_test.exe` — the client's defences: TLS
  against real servers (including certificates that must be refused), the text sanitizer,
  and permessage-deflate against the server encoder's own vectors.

Generation scripts give every run its own directory and check the spoiler's seed: reusing
one directory with `rmtree(ignore_errors=True)` once made later runs read earlier output.

Death link with one copy of the game: generate a two-slot seed (your YAML plus one named
`Probe`, both with `death_link: true`), host it, connect the game as your slot and run
`python rig/deathlink_probe.py Probe`.

Run the game with a separate profile, so testing doesn't touch your real saves and mods:

    dusklight --mods <folder with archipelago.dusk> --user-dir <test profile> --log-dir <logs>

Run a local server (from the Archipelago checkout):

    SKIP_REQUIREMENTS_UPDATE=1 python MultiServer.py --port 38281 <seed>.zip

Turn on the mod's own debug trail with its `debugLog` config var; it writes
`<user dir>/mod_data/com.noahsmaximum.dusklight_archipelago/ap_debug.log`.
