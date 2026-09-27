Dusklight Archipelago adds full Archipelago multiworld support to Dusklight, the Twilight Princess PC port. It's built on the official Dusklight randomizer.
You don't need a separate client and you don't patch anything. Pick Archipelago from the Play menu, enter your server and slot name, and the mod rebuilds your seed in-game with the randomizer's own generator. A multiworld seed plays exactly like a normal rando seed, with the same logic, stage edits, text and progressive items.
Features

* Connects from inside the game. Checks go out as you collect them, and items from other players arrive automatically. Anything you pick up while disconnected gets sent the next time you connect.
* Other players' items show as the box of the game they belong to, with its cover art, on the ground, in Link's hands and in the item text box. Covers come from SteamGridDB with your own free API key, download once per game, and fall back to the Sol if you'd rather not use a key.
* Built-in Archipelago tab (F1):
   * Tracker showing which checks are in logic right now
   * Hint browser where you can request hints
   * Room chat with emoji support
   * Connection status and reconnect
* Optional HUD overlay you can put anywhere on screen. It shows status, checks, received items, hints, chat and death link.
* Death link. Bottled fairies still save you from deaths other players send.
* Works with ws:// and wss://. The mod has its own TLS with proper certificate checks and supports compressed messages.
* Seven ready-made presets (under github releases):
   * Easy: about 150 checks
   * Normal: about 200 checks
   * Hard: about 300 checks
   * Extreme: about 450 checks
   * Ultimate, Hero of Twilight and Hero of Time: every check in the game (about 570) at Normal, Hard and hardest difficulty
* Shuffled Dungeons option (0–9) so you can control how long a run is.
* Runs alongside Dusklight's built-in randomizer without conflicts.

Requirements: put `archipelago.dusk` in your Dusklight mods folder and `tp_dusklight.apworld` in Archipelago's `custom_worlds` folder. You'll also need a YAML: use one of the presets or the included template. For game boxes, a free SteamGridDB API key (optional).
This fork's code, apworld and documentation were written/assisted with Claude Code. I, NoahsMaximum have been directing, reviewing and play-testing every version. The randomizer is built off of TwilitRealm's randomizer. Every release goes through manual and automated tests (each preset's seeds rebuilt in the game's own generator and compared exactly, the TLS and compression suites, builds on every platform), but not every change has been played in game before release, so edge-case bugs might get through. Please [open an issue](https://github.com/noahsmaximum/dusklight-archipelago/issues) if you find anything.
