## Install

1. **Mod** — put `archipelago.dusk` in your Dusklight mods folder:
   - Windows: `%APPDATA%\TwilitRealm\Dusklight\mods`
   - Linux: `~/.local/share/TwilitRealm/Dusklight/mods`
   - macOS: `~/Library/Application Support/TwilitRealm/Dusklight/mods`
2. **Apworld** — put `tp_dusklight.apworld` in your Archipelago `custom_worlds` folder.
3. **YAML** — grab a preset below, or generate a template from the Archipelago Launcher. Set `name:` to your slot name.

Then in Dusklight: use the arrows on the Play button to pick **Archipelago**, start a **new file**, and enter your server address, slot name and password. The seed builds from the server and you play.

Needs Dusklight 2.0.1 or newer. This mod carries its own copy of the randomizer, so Dusklight's built-in Randomizer doesn't need updating or removing; the two don't interfere.

## What's new in 1.5.0

- **Item jingles match the item.** Picking up another player's item plays the fanfare for how important it is to them: the full item-get fanfare for progression, the smaller jingle for useful items, the quick one for filler, and the save menu's "No" sound for traps.
- **Fixed:** after finding another player's item, the next item text box could show that game's cover instead of its own icon.
- **Connecting is sturdier.** Bad data from a room no longer leaves the game stuck on "connecting…": harmless gaps are skipped, and anything that would break the seed is shown as an error in the connection window.
- The apworld is unchanged (1.7.0).

New in 1.4.0: game boxes. Other players' items can show as their game's box with its cover art (SteamGridDB, your own free API key).

## Updating

Update the mod and the apworld **together**. The generating host needs the new apworld for the new presets and options; if the logic data ever differs between a player's mod and the seed, the mod refuses it and says to update. Finish any multiworld already in progress on the versions you started it with.
