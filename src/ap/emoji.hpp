#pragma once

// Emoji for the Archipelago window and the toasts. The game's fonts have no emoji, and mods
// can't add fonts, so emoji are drawn as images from the mod's own bundle: Twemoji, in
// res/emoji/ (tools/fetch_twemoji.py), served to the UI as mod://<mod id>/res/emoji/<file>.png.
//
// No mod services in here, so tools/tls_test.cpp can test it directly.

#include <string>
#include <string_view>
#include <vector>

namespace ap::emoji {

// Must match the id in mod.json: it's how mod:// image sources find the bundle.
inline constexpr std::string_view kModId = "com.noahsmaximum.dusklight_archipelago";

// `rml` is text that's already been escaped for RML. Every emoji sequence we have an image
// for, and every picker :shortcode:, becomes an inline image; everything else passes through
// untouched. Skin-tone modifiers are dropped (the bundle leaves those variants out), so a
// toned emoji shows in the default tone rather than not at all.
std::string emojify(std::string_view rml);

// The player's own text on its way out: picker :shortcodes: become the emoji themselves, so
// every other client (text client, Discord bridges) shows them.
std::string shortcodes_to_unicode(std::string_view text);

struct PickerEmoji {
    const char* name;  // shortcode, without the colons
    const char* file;  // res/emoji/<file>.png, lowercase hex codepoints joined by '-'
};

// The emoji the picker offers, in picker order.
const std::vector<PickerEmoji>& picker();

// mod:// source of res/emoji/<file>.png.
std::string image_source(std::string_view file);

}  // namespace ap::emoji
