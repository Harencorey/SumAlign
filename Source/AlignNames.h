#pragma once
//==============================================================================
// SumAlign - what a track's name says about it. Plain C++ (no JUCE).
//
// Session ALIGN groups tracks by the sound they share. Bleed can fool that (the kick in the bass amp
// mic, a vocal mic in the drum room), so the track name is used as a hint:
//   - family: drums / bass / guitar / vocals / keys, or unknown. In Auto, two tracks of DIFFERENT known
//     families are never linked (bass DI + bass amp stay together; the kick bleeding into the bass amp
//     doesn't pull it into the drums). Unknown names ("Audio 3", "LDC", "DI") can link with anything.
//   - bus: names like "Drum Bus", "Kick Aux", "Verb", "Smash" are sums or effects of other tracks, so
//     an Auto instance on one sits out of the session. (A manual group overrides this.)
//
// Matching works on words: lower case, split at anything that isn't a letter or digit and between
// letters and digits ("FT2" -> "ft" "2", "SnareBot" stays one word, "Snare_Bot" -> "snare" "bot").
//==============================================================================

#include <string>
#include <vector>

namespace sa
{

enum class Family { unknown = 0, drums, bass, guitar, vocals, keys };

inline const char* familyName (Family f)
{
    switch (f)
    {
        case Family::drums:  return "drums";
        case Family::bass:   return "bass";
        case Family::guitar: return "guitar";
        case Family::vocals: return "vocals";
        case Family::keys:   return "keys";
        default:             return "";
    }
}

struct NameInfo
{
    Family family = Family::unknown;
    bool bus = false;
};

inline std::vector<std::string> nameWords (const std::string& name)
{
    std::vector<std::string> words;
    std::string cur;
    int curKind = 0;   // 1 = letters, 2 = digits
    auto flush = [&] { if (! cur.empty()) words.push_back (cur); cur.clear(); curKind = 0; };
    for (unsigned char ch : name)
    {
        int kind = 0;
        char c = (char) ch;
        if (c >= 'A' && c <= 'Z') { c = (char) (c - 'A' + 'a'); kind = 1; }
        else if (c >= 'a' && c <= 'z') kind = 1;
        else if (c >= '0' && c <= '9') kind = 2;
        if (kind == 0) { flush(); continue; }
        if (curKind != 0 && kind != curKind) flush();
        cur += c; curKind = kind;
    }
    flush();
    return words;
}

inline NameInfo classifyTrackName (const std::string& name)
{
    auto in = [] (const std::string& w, std::initializer_list<const char*> list)
    {
        for (auto* s : list) if (w == s) return true;
        return false;
    };
    // a word that starts with one of these (e.g. "snarebot", "kickin", "overheads")
    auto starts = [] (const std::string& w, std::initializer_list<const char*> list)
    {
        for (auto* s : list) { const std::string p (s); if (w.size() >= p.size() && w.compare (0, p.size(), p) == 0) return true; }
        return false;
    };

    bool drumStrong = false, drumWeak = false, bass = false, guitar = false, vocals = false, keys = false, bus = false;
    for (auto& w : nameWords (name))
    {
        // a drum itself: beats every other family ("Bass Drum" is a kick)
        if (in (w, { "bd", "kik", "kck", "sn", "snr", "sd", "tom", "toms", "rt", "ft", "fl", "hh", "hat", "hats", "ride", "rd",
                     "bassdrum", "crash", "cym", "cymbal", "cymbals", "china", "splash", "drum", "drums", "perc", "cajon", "trash" })
            || starts (w, { "kick", "snare", "hihat", "floortom", "racktom", "tom", "drum" }))
            drumStrong = true;
        // room-type mics: drums unless another instrument is named ("Gtr Room" is a guitar)
        else if (in (w, { "oh", "ohl", "ohr", "rm", "amb", "ambience", "ambient", "kit", "trig", "trigger" })
                 || starts (w, { "overhead", "room", "trig" }))
            drumWeak = true;
        else if (in (w, { "bass", "bs", "bas", "bassdi", "bassamp" }) || starts (w, { "bass" }))
            bass = true;
        else if (in (w, { "gtr", "gtrs", "gt", "git", "gtar", "acg", "acoustic" }) || starts (w, { "guitar", "gtr" }))
            guitar = true;
        else if (in (w, { "vox", "vx", "voc", "vocal", "vocals", "lv", "bgv", "bgvs", "bv", "bvs", "harmony", "harm", "choir", "singer" })
                 || starts (w, { "vocal", "vox" }))
            vocals = true;
        else if (in (w, { "piano", "pno", "keys", "key", "organ", "b3", "leslie", "rhodes", "wurli", "wurlitzer", "synth", "clav", "mellotron" }))
            keys = true;

        if (in (w, { "bus", "buss", "busses", "aux", "submix", "stem", "stems", "verb", "reverb", "rev", "return", "rtn", "fx",
                     "master", "print", "smash", "crush", "parallel", "nyc", "sum", "summing", "mix" }))
            bus = true;
    }

    NameInfo info;
    info.bus = bus;
    if (drumStrong)                      info.family = Family::drums;
    else if (bass)                       info.family = Family::bass;
    else if (guitar)                     info.family = Family::guitar;
    else if (vocals)                     info.family = Family::vocals;
    else if (keys)                       info.family = Family::keys;
    else if (drumWeak)                   info.family = Family::drums;
    // "kick" etc. combined with "bass" only in "bass drum" style names, handled by drumStrong above
    return info;
}

// Auto tracks of two different known families are never linked.
inline bool familiesMayLink (Family a, Family b)
{
    return a == Family::unknown || b == Family::unknown || a == b;
}

} // namespace sa
