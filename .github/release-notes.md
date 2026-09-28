## Install

1. **Mod** — put `archipelago.dusk` in your Dusklight mods folder:
   - Windows: `%APPDATA%\TwilitRealm\Dusklight\mods`
   - Linux: `~/.local/share/TwilitRealm/Dusklight/mods`
   - macOS: `~/Library/Application Support/TwilitRealm/Dusklight/mods`
2. **Apworld** — put `tp_dusklight.apworld` in your Archipelago `custom_worlds` folder.
3. **YAML** — grab a preset below, or generate a template from the Archipelago Launcher. Set `name:` to your slot name.

Then in Dusklight: use the arrows on the Play button to pick **Archipelago**, start a **new file**, and enter your server address, slot name and password. The seed builds from the server and you play.

Needs Dusklight 2.0.1 or newer. This mod carries its own copy of the randomizer, so Dusklight's built-in Randomizer doesn't need updating or removing; the two don't interfere.

## What's new in 1.6.0

- **Randomizer update (Twilit Realm's latest).** Logic now tells apart being in a twilight as a wolf or as a human, a Snowpeak Ruins logic fix, missing Castle Town and Ordon object fixes, and no more muted audio after the Forest Temple boss door.
- **Update the mod and the apworld together** (mod 1.6.0, apworld 1.8.0). The logic changed, so each refuses seeds made with the other's old version. Finish runs already in progress on the versions you started them with.

New in 1.5.1: connecting works on Linux ([#2](https://github.com/noahsmaximum/dusklight-archipelago/issues/2)).

## Updating

Update the mod and the apworld **together**. The generating host needs the new apworld for the new presets and options; if the logic data ever differs between a player's mod and the seed, the mod refuses it and says to update. Finish any multiworld already in progress on the versions you started it with.
