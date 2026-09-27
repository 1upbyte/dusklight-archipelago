#pragma once

// Covers for other worlds' items. Each game's box art comes from SteamGridDB with the player's
// own API key, is cached on disk once per game (randomizer/archipelago/covers, shared by every
// room and seed), and is decoded into a texture the item draw hook can use. Without a key, or
// for games SteamGridDB doesn't know, there's no cover and the item stays a Sol.
//
// Game thread only.

#include <dolphin/gx/GXStruct.h>

#include <string>
#include <vector>

namespace ap::covers {

struct Cover {
    GXTexObj texture;          // RGBA8 with mips; its texels live as long as the Cover
    GXColor spine;             // the box's edges
    float aspect = 0.7f;       // the art's width / height
    std::vector<uint8_t> texels;
    // A ResTIMG (header, then RGBA8 texels with mips) of the cover on a transparent square, for
    // the get-item text box's icon picture.
    std::vector<uint8_t> iconTimg;
};

// Every frame: the current settings, then any finished downloads and decodes.
void tick(const std::string& apiKey, bool enabled);

// The games whose items are in this world. Cached covers load; missing ones queue a download.
void want(const std::vector<std::string>& games);

// The game's cover once it's ready, else nullptr (the item draws as a Sol).
const Cover* get(const std::string& game);

// One line for the settings page: what's ready, what's coming, what went wrong.
std::string status();

// Forget the downloaded covers (and what SteamGridDB didn't have) and fetch them again.
void refetch();

}  // namespace ap::covers
