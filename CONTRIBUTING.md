# Contributing

Thanks for looking. Start with [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md): it explains how the
mod and the apworld fit together, maps the code, and lists the rules that are easy to break.

## What's ours and what's upstream

This repo is a fork of [Twilit Realm's randomizer](https://github.com/TwilitRealm/dusklight-randomizer),
and most of it is their code. The Archipelago parts are `src/ap/`, `apworld/`, `presets/`,
`rig/` and most of `tools/`, plus small additions to `generator/logic` (search, fill, world) and
`src/hooks.cpp`. Fixes to the randomizer itself are best sent upstream; we merge upstream with
`git fetch upstream && git merge upstream/main` (merge, don't rebase: the history contains
upstream merges).

`archipelago` is the only branch. Open pull requests against it.

## Setting up

You need CMake 3.25+, Ninja, a C++23 compiler (MSVC from Visual Studio 2022 or later, GCC 13+,
or Clang 17+) and Python 3.11+. The first CMake configure fetches Dusklight's mod SDK and the
dependencies.

```sh
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build --parallel
```

On Windows, `rig/build_mod.cmd` does the same from a Visual Studio environment, and
`rig/build_mod_android.cmd` cross-builds for Android. The build produces:

- `build/mods/archipelago.dusk`: the mod, for your platform.
- `build/tls_test`: host tests for the network client and other pure code.
- `build/ap_gen_test`: rebuilds a seed from slot data exactly as the mod does.

For anything involving the apworld you also need an
[Archipelago](https://github.com/ArchipelagoMW/Archipelago) source checkout (0.6.x) and a Python
environment that runs it. The rig expects them next to the repo as `../ap` and `../venv`; set
`AP_DIR` / `AP_PYTHON` otherwise (see `rig/paths.py`). Build and install the apworld with:

```sh
python tools/build_apworld.py --install <Archipelago>/custom_worlds
```

Re-run it after changing anything under `apworld/` or `generator/data/`.

## Testing

What to run depends on what you touched:

| You changed | Run |
| --- | --- |
| Anything | `build/tls_test` (TLS against real servers, text sanitising, compression, emoji, cover matching); CI runs it too. |
| Logic data or the apworld's logic | `python tools/check_data_version.py build/ap_gen_test` (the mod's and apworld's fingerprints agree), then `python rig/tracker_parity.py presets/<X>.yaml 1 2 3` for several presets (Hard and the Hero presets keep all three twilights), and `python rig/tracker_parity.py presets/Hard.yaml 4 --players 2`. |
| Generation, slot data, placements | `python rig/sweep_dungeons.py presets/*.yaml` (every preset's seeds rebuilt in the game's generator and compared exactly) and `python rig/sweep_dungeons.py` (80 seeds across Shuffled Dungeons x key settings). |
| Options | `python tools/make_template.py <Archipelago> --python <its python>` to regenerate `presets/Template.yaml`, and `python rig/count_checks.py presets/*.yaml` if check counts could move. |
| The network client | `build/tls_test`; `rig/ws_proxy.py` shows every message between the game and a server. |
| Anything you can see in game | Play it. `rig/README.md` explains running the game with a test profile and a local server. |

Say in the pull request what you ran, and whether the change has been played in game.

## Code

- Match the surrounding code: naming, layout, comment density. Comments say *why*, especially
  for game behaviour that isn't obvious ("text boxes are reallocated at the same address").
- The mode's code is split by area under `src/ap/mode/`. Put new code in the file for its area;
  state shared between files goes in `mode/internal.hpp`, everything else stays `static` in its
  file.
- Pure logic that doesn't need the game (matching, parsing, image processing) goes in files free
  of Dusklight headers, so `tls_test` can test it on a desktop.
- Treat everything from the server as untrusted (see the architecture doc).
- Never commit API keys. `rig/cover_probe.py` reads a SteamGridDB key from `SGDB_KEY` or the
  git-ignored `rig/sgdb_key.txt`.

## Releasing (maintainers)

1. Bump `ARCHIPELAGO_VERSION` in `CMakeLists.txt` (it drives `mod.json`). If the logic data or
   the apworld's options changed, bump `world_version` in `apworld/tp_dusklight/archipelago.json`
   and the presets' `requires`.
2. Rewrite the "What's new" section of `.github/release-notes.md`, keeping the install guide at
   the top. Keep it short.
3. Refresh `src/ap/ca_bundle.pem` from [curl.se](https://curl.se/docs/caextract.html) if it's
   been a while.
4. If options changed, regenerate `presets/Template.yaml` and re-measure the presets (above).
5. Commit, push, tag `vX.Y.Z` and push the tag. CI builds every platform and publishes the
   release with the bundle, the apworld and the presets.
6. Download the release's assets and check the versions they report.
7. Update `.github/mod-page.md` (the mod page text) for anything new.
