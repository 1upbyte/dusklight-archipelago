"""AP options generated from the randomizer's own settings_list.yaml.

Every exposed setting keeps the randomizer's exact name and option labels on the wire
(slot_data["settings"]), so the in-game generator reproduces the same world.
"""
from __future__ import annotations

import re
from dataclasses import make_dataclass
from typing import Any

from Options import (Choice, DeathLink, DefaultOnToggle, PerGameCommonOptions, Range,
                     StartInventoryPool, Toggle)

from . import data

# Settings the AP world pins instead of exposing, with the value it pins them to.
# Entrance shuffle and hints are decided by the in-game generator from local data only,
# which can't see other worlds, so they stay off; logic rules are always "all reachable".
FORCED: dict[str, str] = {
    "Logic Rules": "All Locations Reachable",
    "Unrequired Dungeons Are Barren": "Off",
    "Randomize Starting Spawn": "Off",
    "Randomize Dungeon Entrances": "Off",
    "Randomize Boss Entrances": "Off",
    "Randomize Grotto Entrances": "Off",
    "Randomize Cave Entrances": "Off",
    "Randomize Interior Entrances": "Off",
    "Randomize Overworld Entrances": "Off",
    "Decouple Double Door Entrances": "Off",
    "Decouple Entrances": "Off",
    "Number of Path Hints": "0",
    "Path Hints on Midna": "Off",
    "Path Hints on Hint Signs": "Off",
    "Number of Barren Hints": "0",
    "Barren Hints on Midna": "Off",
    "Barren Hints on Hint Signs": "Off",
    "Number of Item Hints": "0",
    "Item Hints on Midna": "Off",
    "Item Hints on Hint Signs": "Off",
    "Number of Location Hints": "0",
    "Location Hints on Midna": "Off",
    "Location Hints on Hint Signs": "Off",
    "Prioritize Remote Location Hints": "Off",
}


def option_key(setting_name: str) -> str:
    return re.sub(r"[^a-z0-9]+", "_", setting_name.lower().replace("'", "")).strip("_")


def _option_attr(label: str) -> str:
    return "option_" + option_key(label)


# The randomizer's defaults are a gentle first seed. Archipelago's are "everything that can be
# a check is one", so a template or YAML that leaves these out gets the full game; the
# presets set every one of them explicitly.
MAX_CHECK_DEFAULTS: dict[str, str] = {
    "Golden Bugs": "On",
    "Sky Characters": "On",
    "Gifts From NPCs": "On",
    "Shop Items": "On",
    "Hidden Skills": "On",
    "Hidden Rupees": "On",
    "Freestanding Rupees": "On",
    "Poe Souls": "All",
    "Small Keys": "Own Dungeon",
    "Big Keys": "Own Dungeon",
    "Maps and Compasses": "Own Dungeon",
}


def _numeric_doc(info: data.SettingInfo) -> str:
    """Numeric settings carry no text upstream, but each is the count for one choice of a
    "... Requirements" setting (e.g. Hyrule Barrier Fused Shadows), so say that."""
    for parent in data.settings().values():
        prefix = parent.name.removesuffix(" Requirements")
        if parent.name.endswith(" Requirements") and info.name.startswith(prefix + " "):
            choice = info.name[len(prefix) + 1:]
            if choice in parent.options:
                return (f"How many {choice.lower()} are needed when {parent.name} is set to "
                        f"{choice}.")
    return f"{info.name}."


def _doc(info: data.SettingInfo) -> str:
    if info.numeric:
        return _numeric_doc(info)
    lines = [f"{info.name}."]
    described = [(label, info.descriptions[label]) for label in info.options
                 if label in info.descriptions]
    if described:
        lines.append("")
        lines.extend(f"**{label}:** {text}" for label, text in described)
    return "\n".join(lines)


def _make_option(info: data.SettingInfo) -> type:
    doc = _doc(info)
    default = MAX_CHECK_DEFAULTS.get(info.name, info.default)
    if info.numeric:
        values = [int(o) for o in info.options]
        return type(option_key(info.name).title().replace("_", ""), (Range,), {
            "__doc__": doc,
            "display_name": info.name,
            "range_start": min(values),
            "range_end": max(values),
            "default": int(default),
        })
    if info.options == ["Off", "On"]:
        base = DefaultOnToggle if default == "On" else Toggle
        return type(option_key(info.name).title().replace("_", ""), (base,), {
            "__doc__": doc,
            "display_name": info.name,
        })
    attrs: dict[str, Any] = {"__doc__": doc, "display_name": info.name}
    for i, label in enumerate(info.options):
        attrs[_option_attr(label)] = i
    attrs["default"] = info.options.index(default)
    return type(option_key(info.name).title().replace("_", ""), (Choice,), attrs)


class ShuffledDungeons(Range):
    """How many of the nine dungeons have their contents shuffled into the multiworld.

    The others, picked at random for each seed, keep their vanilla chests, keys, maps and
    big items. You still play them, and logic still expects their items, but they aren't
    checks. Lower this for a shorter game: each dungeon is roughly 8 to 22 checks."""
    display_name = "Shuffled Dungeons"
    range_start = 0
    range_end = len(data.DUNGEONS)
    default = len(data.DUNGEONS)


class CollectDungeonOnCompletion(Toggle):
    """When you beat a dungeon's boss, every check still left in that dungeon is collected at
    once: other players get their items right away, and yours arrive in-game. Chests you go
    back to afterwards hold a green rupee. Hyrule Castle is the goal, so it isn't included."""
    display_name = "Collect Dungeon on Completion"


EXPOSED: dict[str, str] = {}  # option attribute -> setting name
_fields: list[tuple[str, type]] = []
for _info in data.settings().values():
    if _info.name in FORCED:
        continue
    _key = option_key(_info.name)
    EXPOSED[_key] = _info.name
    _fields.append((_key, _make_option(_info)))


# Archipelago-only: decided here, sent to the mod as explicit placements (see fill_slot_data).
_fields.append(("shuffled_dungeons", ShuffledDungeons))
# Archipelago-only: carried out by the mod (see fill_slot_data, "collect_dungeons").
_fields.append(("collect_dungeon_on_completion", CollectDungeonOnCompletion))
_fields.append(("start_inventory_from_pool", StartInventoryPool))
# Archipelago's own option, not a randomizer setting: the mod joins the DeathLink channel.
_fields.append(("death_link", DeathLink))

TPOptions = make_dataclass("TPOptions", _fields, bases=(PerGameCommonOptions,))


def resolve_settings(options: Any) -> dict[str, str]:
    """Randomizer setting name -> option label, for every setting the generator knows."""
    out: dict[str, str] = {}
    for info in data.settings().values():
        if info.name in FORCED:
            out[info.name] = FORCED[info.name]
            continue
        opt = getattr(options, option_key(info.name))
        if info.numeric:
            out[info.name] = str(int(opt.value))
        elif info.options == ["Off", "On"]:
            out[info.name] = "On" if opt.value else "Off"
        else:
            out[info.name] = info.options[int(opt.value)]
    return out
