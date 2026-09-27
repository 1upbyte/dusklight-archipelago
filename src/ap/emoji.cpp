#include "emoji.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace ap::emoji {

namespace {

constexpr const char* kFiles[] = {
#include "emoji_table.inc"
};

const std::unordered_set<std::string_view>& files() {
    static const std::unordered_set<std::string_view> set(std::begin(kFiles), std::end(kFiles));
    return set;
}

// Discord-style names where Discord has one.
const std::vector<PickerEmoji> kPicker = {
    {"smile", "1f604"}, {"grin", "1f601"}, {"joy", "1f602"}, {"rofl", "1f923"},
    {"sweat_smile", "1f605"}, {"wink", "1f609"}, {"blush", "1f60a"}, {"heart_eyes", "1f60d"},
    {"sunglasses", "1f60e"}, {"thinking", "1f914"}, {"upside_down", "1f643"},
    {"neutral_face", "1f610"}, {"unamused", "1f612"}, {"rolling_eyes", "1f644"},
    {"grimacing", "1f62c"}, {"flushed", "1f633"}, {"pleading_face", "1f97a"}, {"cry", "1f622"},
    {"sob", "1f62d"}, {"scream", "1f631"}, {"rage", "1f621"}, {"skull", "1f480"},
    {"clown", "1f921"}, {"ghost", "1f47b"}, {"partying_face", "1f973"}, {"sleeping", "1f634"},
    {"nerd", "1f913"}, {"melting_face", "1fae0"}, {"saluting_face", "1fae1"}, {"eyes", "1f440"},
    {"thumbsup", "1f44d"}, {"thumbsdown", "1f44e"}, {"clap", "1f44f"}, {"wave", "1f44b"},
    {"pray", "1f64f"}, {"muscle", "1f4aa"}, {"ok_hand", "1f44c"}, {"v", "270c"},
    {"crossed_fingers", "1f91e"}, {"raised_hands", "1f64c"}, {"handshake", "1f91d"},
    {"facepalm", "1f926"}, {"shrug", "1f937"}, {"heart", "2764"}, {"broken_heart", "1f494"},
    {"sparkling_heart", "1f496"}, {"fire", "1f525"}, {"sparkles", "2728"}, {"star", "2b50"},
    {"boom", "1f4a5"}, {"100", "1f4af"}, {"tada", "1f389"}, {"trophy", "1f3c6"},
    {"crown", "1f451"}, {"gem", "1f48e"}, {"moneybag", "1f4b0"}, {"key", "1f511"},
    {"lock", "1f512"}, {"crossed_swords", "2694"}, {"shield", "1f6e1"},
    {"bow_and_arrow", "1f3f9"}, {"bomb", "1f4a3"}, {"wolf", "1f43a"}, {"horse", "1f434"},
    {"fish", "1f41f"}, {"bug", "1f41b"}, {"goat", "1f410"}, {"cheese", "1f9c0"},
    {"jack_o_lantern", "1f383"}, {"mushroom", "1f344"}, {"hourglass", "231b"}, {"zzz", "1f4a4"},
    {"warning", "26a0"}, {"question", "2753"}, {"exclamation", "2757"}, {"x", "274c"},
    {"white_check_mark", "2705"}, {"sun", "2600"}, {"crescent_moon", "1f319"},
    {"rainbow", "1f308"},
};

const std::unordered_map<std::string_view, std::string_view>& shortcodes() {
    static const auto map = [] {
        std::unordered_map<std::string_view, std::string_view> m;
        for (const auto& e : kPicker) {
            m.emplace(e.name, e.file);
        }
        return m;
    }();
    return map;
}

// Next codepoint of UTF-8 `s` at `i`; `len` receives its byte length. Malformed input comes
// back one byte at a time (as that byte), so it's copied through rather than lost.
uint32_t decode(std::string_view s, size_t i, size_t& len) {
    const auto b0 = static_cast<unsigned char>(s[i]);
    auto cont = [&](size_t k) {
        return i + k < s.size() && (static_cast<unsigned char>(s[i + k]) & 0xC0) == 0x80;
    };
    auto cb = [&](size_t k) { return static_cast<uint32_t>(s[i + k] & 0x3F); };
    if (b0 < 0x80) {
        len = 1;
        return b0;
    }
    if ((b0 & 0xE0) == 0xC0 && cont(1)) {
        len = 2;
        return ((b0 & 0x1F) << 6) | cb(1);
    }
    if ((b0 & 0xF0) == 0xE0 && cont(1) && cont(2)) {
        len = 3;
        return ((b0 & 0x0F) << 12) | (cb(1) << 6) | cb(2);
    }
    if ((b0 & 0xF8) == 0xF0 && cont(1) && cont(2) && cont(3)) {
        len = 4;
        return ((b0 & 0x07) << 18) | (cb(1) << 12) | (cb(2) << 6) | cb(3);
    }
    len = 1;
    return b0;
}

void encode(uint32_t cp, std::string& out) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

constexpr uint32_t kZwj = 0x200D;
constexpr uint32_t kVs16 = 0xFE0F;
bool is_skin_tone(uint32_t cp) { return cp >= 0x1F3FB && cp <= 0x1F3FF; }

// Twemoji's file naming: variation selector 16 is dropped unless the sequence has a ZWJ.
std::string file_key(const uint32_t* cps, size_t n) {
    bool zwj = false;
    for (size_t k = 0; k < n; ++k) {
        zwj = zwj || cps[k] == kZwj;
    }
    std::string key;
    char buf[12];
    for (size_t k = 0; k < n; ++k) {
        if (is_skin_tone(cps[k]) || (!zwj && cps[k] == kVs16)) {
            continue;
        }
        std::snprintf(buf, sizeof buf, "%x", cps[k]);
        if (!key.empty()) {
            key += '-';
        }
        key += buf;
    }
    return key;
}

std::string image_tag(std::string_view file) {
    return "<img class=\"ap-emoji\" src=\"" + image_source(file) +
           "\" style=\"width: 1.3em; height: 1.3em; vertical-align: -0.25em;\"/>";
}

bool shortcode_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '+' || c == '-';
}

// If `s` at `i` starts a picker :shortcode:, its file and the byte length to skip.
bool match_shortcode(std::string_view s, size_t i, std::string_view& file, size_t& len) {
    if (s[i] != ':') {
        return false;
    }
    size_t j = i + 1;
    while (j < s.size() && j - i <= 32 && shortcode_char(s[j])) {
        ++j;
    }
    if (j >= s.size() || s[j] != ':' || j == i + 1) {
        return false;
    }
    const auto it = shortcodes().find(s.substr(i + 1, j - i - 1));
    if (it == shortcodes().end()) {
        return false;
    }
    file = it->second;
    len = j - i + 1;
    return true;
}

}  // namespace

std::string image_source(std::string_view file) {
    return "mod://" + std::string{kModId} + "/res/emoji/" + std::string{file} + ".png";
}

const std::vector<PickerEmoji>& picker() {
    return kPicker;
}

std::string emojify(std::string_view s) {
    constexpr size_t kMaxSequence = 10;  // longest Twemoji sequence is 7 codepoints (+ tones)
    std::string out;
    out.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        std::string_view file;
        size_t skip = 0;
        if (match_shortcode(s, i, file, skip)) {
            out += image_tag(file);
            i += skip;
            continue;
        }
        size_t firstLen = 0;
        const uint32_t first = decode(s, i, firstLen);
        // Emoji start past ASCII, except keycaps (#, * and digits followed by U+20E3).
        const bool keycapStart = first == '#' || first == '*' || (first >= '0' && first <= '9');
        if (first < 0x80 && !keycapStart) {
            out += static_cast<char>(first);
            ++i;
            continue;
        }
        std::array<uint32_t, kMaxSequence> cps{};
        std::array<size_t, kMaxSequence + 1> ends{};
        size_t n = 0;
        for (size_t at = i; at < s.size() && n < kMaxSequence;) {
            size_t len = 0;
            const uint32_t cp = decode(s, at, len);
            if (n > 0 && cp < 0x80) {
                break;
            }
            cps[n++] = cp;
            at += len;
            ends[n] = at;
        }
        bool matched = false;
        for (size_t take = n; take > 0 && !matched; --take) {
            if (keycapStart && (take < 2 || (cps[1] != 0x20E3 && (take < 3 || cps[2] != 0x20E3)))) {
                continue;
            }
            const std::string key = file_key(cps.data(), take);
            if (!key.empty() && files().contains(key)) {
                out += image_tag(key);
                i = ends[take];
                matched = true;
            }
        }
        if (!matched) {
            out.append(s.substr(i, firstLen));
            i += firstLen;
        }
    }
    return out;
}

std::string glyph(std::string_view file) {
    std::string out;
    size_t count = 0;
    for (size_t start = 0; start <= file.size();) {
        const size_t dash = std::min(file.find('-', start), file.size());
        encode(static_cast<uint32_t>(
                   std::stoul(std::string{file.substr(start, dash - start)}, nullptr, 16)),
            out);
        ++count;
        start = dash + 1;
    }
    // Single codepoints from the older blocks (heart, sun, crossed swords...) read as plain
    // text symbols unless asked for emoji presentation.
    if (count == 1 && file.size() <= 4) {
        encode(kVs16, out);
    }
    return out;
}

std::string shortcodes_to_unicode(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        std::string_view file;
        size_t skip = 0;
        if (!match_shortcode(s, i, file, skip)) {
            out += s[i++];
            continue;
        }
        out += glyph(file);
        i += skip;
    }
    return out;
}

}  // namespace ap::emoji
