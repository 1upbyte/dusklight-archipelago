<p align="center">
  <img src="res/banner.png" alt="Dusklight Archipelago logo" width="1300">
</p>

# Dusklight Archipelago

[Archipelago](https://archipelago.gg) multiworld support for [Dusklight](https://github.com/TwilitRealm/dusklight),
built on top of the [official Dusklight randomizer](https://github.com/TwilitRealm/dusklight-randomizer).

The mod connects to an Archipelago server from inside the game and rebuilds the seed with the
randomizer's own generator, so a multiworld seed plays exactly like a normal randomizer seed:
same logic, same stage edits, same text, same progressive items. There is no separate client to
run and nothing to patch.

## Playing

You need three things:

1. **The mod.** Copy `archipelago.dusk` into your Dusklight mods folder:
   - Windows: `%APPDATA%\TwilitRealm\Dusklight\mods`
   - Linux: `~/.local/share/TwilitRealm/Dusklight/mods`
   - macOS: `~/Library/Application Support/TwilitRealm/Dusklight/mods`

   It carries its own copy of the randomizer, so Dusklight's built-in Randomizer doesn't need
   updating or removing; the two don't interfere.
2. **The apworld.** Put `tp_dusklight.apworld` into your Archipelago install's `custom_worlds`
   folder. Whoever generates the multiworld needs it; players who only play need it for the
   tracker and text client.
3. **A YAML.** Start from a preset below, or copy `Template.yaml` from the release — the
   full template with every option explained. You can also generate it
   from the Archipelago Launcher; if you just installed or updated the apworld, restart the
   Launcher first, because it keeps using the apworld it loaded when it started.

Easy, Normal, Hard, Extreme and the template's default have `logic_transform_anywhere` on, so
logic may expect you to transform where NPCs can see you. The game normally refuses that; on
those saves the mod allows it by itself, so there's no Dusklight setting to change.

Then:

1. Start Dusklight. On the Play button, use the arrows to pick **Archipelago**, and start it.
2. Create a **new file**. A connection window opens: enter the server (`archipelago.gg:12345`
   or `localhost:38281`), your slot name from the YAML, and the room password if there is one.
3. Press **Connect and start**. The seed is built from the server's data, which takes a few
   seconds, and then you continue to name entry as usual.
4. Play. Checks are sent as you collect them, and items other players find for you arrive
   automatically. Items belonging to other worlds appear as a Sol (or as their game's box, see
   below) and say who they belong to.

Loading an existing Archipelago save reconnects by itself. If the room moved to a different
port, open the **Archipelago** tab in the menu bar (F1), change the server there, and press
Reconnect. That tab also shows connection status, items received and checks sent.

Anything you collect while disconnected is sent the next time you connect, and items you were
given while away arrive when you load the save.

### Servers and encryption

Both `ws://` and `wss://` work. The mod speaks WebSocket itself over a plain TCP socket, and
wraps that in its own TLS for `wss://`, so nothing depends on the host's WebSocket support. It
also takes the compressed messages Archipelago servers send (`permessage-deflate`), so they
don't warn that the client doesn't support compression.

Type the room address the way Archipelago gives it to you (`archipelago.gg:12345`). Rooms are
served either encrypted or plain, never both, so the mod tries the likely one first — TLS for
a remote server, plain for `localhost` — and falls back to the other if that is refused. You
can force one by typing the scheme yourself: `wss://archipelago.gg:12345`.

Server certificates are checked against a list of root authorities built into the mod, so a
room with an expired, self-signed or mismatched certificate is refused rather than silently
trusted. If you run your own server with a self-signed certificate, connect to it over plain
`ws://` instead.

### Death link

Set `death_link: true` in your YAML and you share deaths with everyone else in the
multiworld who has it on: when one of you dies, you all do. A bottled fairy still saves
you from a death someone else sends, just as it would from your own, and a fairy save
doesn't count as dying. A death that arrives during a cutscene or conversation waits until
it's over.

You can switch it on or off for a save from the **Archipelago** tab in the menu bar (F1),
whatever the YAML said.

### Tracker, hints and messages

The **Archipelago** tab in the menu bar (F1) has more pages for use mid-run:

- **Tracker** shows how many of your checks are done and which ones you can do right now. It
  opens on everything in logic; pick a region to see all of its checks. A green dot is in
  logic, a hollow dot isn't yet, and gray, crossed-out text is done. The logic is the seed's
  own, worked out in the background from the items you have, so "in logic" means the same
  thing it means to Archipelago.
- **Hints** lists every hint that involves you: where your items are, and which of other
  players' items are in your world. It shows your hint points and what a hint costs, and lets
  you ask for one by item or by location. For hints on your own items you can set Priority,
  No priority or Avoid, like in Archipelago's text client. New hints also pop up as they arrive.
- **Messages** is everything the room says, newest first, in Archipelago's usual colors: items
  found and sent, hints, chat. Type a message and press Send to chat, or send a server command
  such as `!remaining`. Emoji show as pictures here and in pop-ups; the Emoji button adds one
  to your message as a code like `:joy:`, which goes out as the emoji itself.

The tracker needs the save to have connected to its room, which it does by itself when you load
it.

### Overlay

The **Overlay** page of the Archipelago tab turns on a small panel that stays on screen while you
play, drawn in the game's own HUD so it hides whenever the HUD does. Pick what it shows
(connection status, checks and what's in logic, items you just received, open hints, recent
chat, death link) and where: any corner or either side, with its distance from the edge, size
and background. It starts switched off.

### Game boxes

Items that belong to other worlds can show as the box of the game they're for, with its cover
art, instead of a Sol: leaning back as it turns on the ground or in a shop, and upright in
Link's hands when he picks it up. Covers come from [SteamGridDB](https://www.steamgriddb.com)
with your own API key, which is free: sign in there, open Preferences, then API. The first time you play, the mod asks whether you want
game boxes (and takes the key) or Sols; after that it's under **Other players' items** on the
Status page of the Archipelago tab (F1). Each game's cover downloads once and is kept in
`randomizer/archipelago/covers` for every room after that. Games SteamGridDB doesn't have (and
the ones that only exist inside Archipelago) stay Sols, as does everything when there's no key.
To use your own art for a game, put a PNG or JPG named after the game (as the room lists it) in
`randomizer/archipelago/covers/custom`.

### Collect Dungeon on Completion

Set `collect_dungeon_on_completion: true` in your YAML and beating a dungeon's boss collects
every check still left in that dungeon: other players get their items at once, and yours arrive
in-game without the usual fanfares, with one pop-up listing them. Chests you go back to
afterwards hold a green rupee, so nothing is given twice. It counts from the boss's heart
container or the dungeon's reward, whichever you get first, and skips Hyrule Castle (beating
its boss is the goal).

Picking up another player's item plays a jingle for how important it is to them, going by
Archipelago's item classification: the full item-get fanfare for progression, the heart-piece
jingle for useful items, the rupee jingle for filler, and the save menu's "No" sound for traps.

## Presets

`presets/` holds seven ready-made YAMLs, each verified to generate and to rebuild exactly in
the in-game generator. The first four step up in both length and difficulty; the last three
are the whole game at rising difficulty. Check counts are averages: which dungeons stay
unshuffled is random per seed, and dungeons differ in size.

| Preset | Checks | Difficulty | What it is |
| --- | --- | --- | --- |
| Easy | ~150 | Easy | Prologue, Midna's Desperate Hour and all three twilights done; overworld chests and freestanding items plus one dungeon; plentiful pool, no traps, castle open. |
| Normal | ~200 | Normal | Prologue skipped, twilights mostly intact, keys move between dungeons, hidden skills shuffled, three dungeons, a few traps, castle wants four dungeons. |
| Hard | ~300 | Hard | Nothing skipped, keys anywhere, golden bugs, sky characters, hidden skills and shops shuffled, six dungeons, many traps, double damage. |
| Extreme | ~450 | Extreme | Hard plus a minimal pool, one-hit kills, bonks that hurt, traps everywhere, eight dungeons, and a castle that wants all eight dungeons and all 60 poe souls. |
| Ultimate | ~570 | Normal | Every possible check — all nine dungeons, every bug, sky character, gift, shop item, hidden skill, rupee and poe — at Normal's difficulty. |
| Hero of Twilight | ~570 | Hard | Every possible check at Hard's difficulty. |
| Hero of Time | ~570 | Hardest | Every possible check with Extreme's punishment, and every piece of junk replaced by a trap. |

Easy, Normal, Hard and Extreme start you with the Shadow Crystal, so you can turn into a wolf
from the beginning, and let you transform in front of NPCs (see above). Ultimate, Hero of
Twilight and Hero of Time play it straight: you find the crystal and transform where the game
allows.

Every preset keeps other players' important items out of Hyrule Castle (see below), and Easy,
Normal and Hard raise `progression_balancing` (90, 80, 70) so this game's progression turns up
earlier and there's less waiting in the long tail.

Copy one into your Archipelago `Players` folder and set `name:` to your slot name. The
release also carries `Template.yaml`, the full template with every option explained.

## Options

The YAML options are generated from the randomizer's own settings, so they match the names in
the in-game randomizer menus, and each one is explained in the template with the randomizer's
own description.

Left unset, an option gives you the most checks it can: every optional shuffle is on, every
poe soul is a check, and dungeon items stay in their own dungeon. That's the whole game, so
for anything shorter start from a preset.

**Shuffled Dungeons** (0–9) is this world's own: how many dungeons have their contents
shuffled into the multiworld. The rest, picked at random for each seed, keep their vanilla
chests, keys, maps and big items. You still play them and logic still expects their items, but
they aren't checks, which is how the shorter presets get down to their size. The spoiler log
lists which dungeons stayed vanilla.

**Every preset excludes Hyrule Castle.** It's the final dungeon, so an item another player
needs from there only turns up at the very end of your game, and they'd wait on your whole
run for it. Excluded checks still exist; they just never hold an item another player needs
(a dungeon's own keys can still be inside it when keys stay in their dungeon). Each dungeon
is a location group, so `exclude_locations: [Hyrule Castle, Palace of Twilight]` works. The
template and any YAML that leaves the option out exclude nothing.

A few options are fixed by this world:

- **Entrance randomization** and **randomized starting spawn** are off. The in-game generator
  only sees your own world, so it can't place entrances consistently with the multiworld yet.
- **In-game hints** (hint signs, Midna hints) are off, because the randomizer's hint generator
  can't see other players' worlds. Use Archipelago's own hint system instead.
- **Unrequired dungeons are barren** is off, and logic is always "all locations reachable".

## Building

```sh
git clone https://github.com/noahsmaximum/dusklight-archipelago
cd dusklight-archipelago
cmake -B build
cmake --build build --parallel
```

`build/mods/archipelago.dusk` is the mod. Build the apworld with:

```sh
python tools/build_apworld.py          # writes build/tp_dusklight.apworld
python tools/build_apworld.py --install <Archipelago>/custom_worlds
```

The apworld vendors the randomizer's own YAML data (`generator/data`) and implements the same
logic in Python, so its locations, items and rules stay in step with the mod. Re-run
`tools/build_apworld.py` after changing anything under `generator/data`.

`tools/ap_gen_test.cpp` builds an `ap_gen_test` executable that rebuilds a seed from a saved
`slot_data.json` exactly like the mod does, which is the quickest way to check generation
changes without launching the game.

`tools/tls_test.cpp` builds a `tls_test` executable that drives the mod's TLS client
(`src/ap/tls.cpp`) over ordinary sockets against real servers, checking both that valid
certificates are accepted and that expired, self-signed, untrusted and mismatched ones are
refused. Run it with no arguments for the default suite.

The trusted roots in `src/ap/ca_bundle.pem` come from
[curl.se/docs/caextract.html](https://curl.se/docs/caextract.html) (Mozilla's list). Replace
that file to refresh them; nothing else needs to change.

## Credits

The randomizer, its logic data and its generator are by [Twilit Realm](https://github.com/TwilitRealm).
This fork adds the Archipelago game mode, the network client and the apworld.

Emoji graphics are [Twemoji](https://github.com/jdecked/twemoji), licensed under
[CC-BY 4.0](https://creativecommons.org/licenses/by/4.0/) (see `res/emoji/LICENSE.txt`), and the
emoji font is [Twemoji Mozilla](https://github.com/mozilla/twemoji-colr) (Apache 2.0, graphics
CC-BY 4.0; see `res/fonts/LICENSE.txt`). Images are decoded with
[stb_image](https://github.com/nothings/stb) (public domain). Box art comes from
[SteamGridDB](https://www.steamgriddb.com) at play time and isn't part of this project.

## AI disclosure

This fork's code, apworld and documentation were written/assisted with Claude Code. I, NoahsMaximum have been directing, reviewing and play-testing every version.
The randomizer is built off of TwilitRealm's randomizer. Every release goes through manual and automated tests (each preset's
seeds rebuilt in the game's own generator and compared exactly, the TLS and compression suites,
builds on every platform), but not every change has been played in game before release, so
edge-case bugs might get through. Please [open an issue](https://github.com/noahsmaximum/dusklight-archipelago/issues) if you find anything.
