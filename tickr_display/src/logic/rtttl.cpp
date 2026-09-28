#include "rtttl.h"
#include <string.h>

// Note frequencies (Hz) for octaves 4..7, index = (octave - 4) * 12 + note,
// note: 1 = c, 2 = c#, 3 = d, ... 12 = b. Index 0 is unused (pause).
static const uint16_t NOTE_FREQ[49] = { 0,
    262,  277,  294,  311,  330,  349,  370,  392,  415,  440,  466,  494,
    523,  554,  587,  622,  659,  698,  740,  784,  831,  880,  932,  988,
    1047, 1109, 1175, 1245, 1319, 1397, 1480, 1568, 1661, 1760, 1865, 1976,
    2093, 2217, 2349, 2489, 2637, 2794, 2960, 3136, 3322, 3520, 3729, 3951
};

static const uint8_t  RTTTL_MIN_OCTAVE = 4;
static const uint8_t  RTTTL_MAX_OCTAVE = 7;
static const uint16_t RTTTL_MIN_BPM    = 25;
static const uint16_t RTTTL_MAX_BPM    = 900;

static inline bool is_space(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static inline bool is_digit(char c) {
    return c >= '0' && c <= '9';
}

static inline char to_lower(char c) {
    return (c >= 'A' && c <= 'Z') ? (char)(c + ('a' - 'A')) : c;
}

static inline void skip_spaces(const char** p, const char* end) {
    while (*p < end && is_space(**p)) (*p)++;
}

// Parses up to `max_digits` decimal digits. Returns false if no digit found.
static bool parse_uint(const char** p, const char* end, unsigned max_digits, uint32_t* out) {
    uint32_t v = 0;
    unsigned n = 0;
    while (*p < end && is_digit(**p)) {
        if (++n > max_digits) return false;
        v = v * 10u + (uint32_t)(**p - '0');
        (*p)++;
    }
    if (n == 0) return false;
    *out = v;
    return true;
}

static bool valid_duration(uint32_t d) {
    return d == 1 || d == 2 || d == 4 || d == 8 || d == 16 || d == 32;
}

// Parses a single note token starting at *p (leading spaces already skipped).
// On success advances *p to the character right after the token (',' or end).
static bool parse_note_token(const char** p, const char* end, const RtttlSong* song, RtttlNote* out) {
    uint32_t dur = song->default_dur;
    uint32_t oct = song->default_oct;
    uint8_t  note = 0;
    bool dotted = false;

    if (is_digit(**p)) {
        if (!parse_uint(p, end, 2, &dur)) return false;
        if (!valid_duration(dur)) return false;
    }
    if (*p >= end) return false;

    switch (to_lower(**p)) {
        case 'c': note = 1;  break;
        case 'd': note = 3;  break;
        case 'e': note = 5;  break;
        case 'f': note = 6;  break;
        case 'g': note = 8;  break;
        case 'a': note = 10; break;
        case 'h': // German notation
        case 'b': note = 12; break;
        case 'p': note = 0;  break;
        default: return false;
    }
    (*p)++;

    if (*p < end && **p == '#') {
        if (note == 0) return false;   // "p#" makes no sense
        note++;
        (*p)++;
    }
    if (*p < end && **p == '.') {
        dotted = true;
        (*p)++;
    }
    if (*p < end && is_digit(**p)) {
        oct = (uint32_t)(**p - '0');
        if (oct < RTTTL_MIN_OCTAVE || oct > RTTTL_MAX_OCTAVE) return false;
        (*p)++;
    }
    if (*p < end && **p == '.') {
        if (dotted) return false;      // two dots
        dotted = true;
        (*p)++;
    }
    skip_spaces(p, end);
    if (*p < end && **p != ',') return false;

    uint32_t ms = song->wholenote_ms / dur;
    if (dotted) ms += ms / 2;
    if (ms > 0xFFFFu) ms = 0xFFFFu;

    uint16_t freq = 0;
    if (note != 0) {
        uint32_t idx = (oct - RTTTL_MIN_OCTAVE) * 12u + note;   // note may be 13 for b#/h#
        if (idx > 48) idx = 48;
        freq = NOTE_FREQ[idx];
    }
    out->freq_hz = freq;
    out->duration_ms = (uint16_t)ms;
    return true;
}

bool rtttl_looks_like(const char* s) {
    if (!s) return false;
    size_t len = strnlen(s, RTTTL_MAX_LEN + 1);
    if (len < 3 || len > RTTTL_MAX_LEN) return false;
    int colons = 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == ':') colons++;
        else if (c < 0x20 || c > 0x7E) return false;   // non-printable / non-ASCII
    }
    return colons == 2;
}

bool rtttl_parse(const char* s, RtttlSong* song) {
    if (!s || !song) return false;
    memset(song, 0, sizeof(*song));

    size_t len = strnlen(s, RTTTL_MAX_LEN + 1);
    if (len == 0 || len > RTTTL_MAX_LEN) return false;

    const char* p = s;
    const char* end = s + len;

    // --- name -------------------------------------------------------------
    const char* colon = (const char*)memchr(p, ':', (size_t)(end - p));
    if (!colon) return false;
    {
        const char* ns = p;
        const char* ne = colon;
        while (ns < ne && is_space(*ns)) ns++;
        while (ne > ns && is_space(*(ne - 1))) ne--;
        size_t nlen = (size_t)(ne - ns);
        if (nlen > RTTTL_MAX_NAME_LEN) return false;
        for (size_t i = 0; i < nlen; i++) {
            unsigned char c = (unsigned char)ns[i];
            if (c < 0x20 || c > 0x7E) return false;
        }
        memcpy(song->name, ns, nlen);
        song->name[nlen] = '\0';
    }
    p = colon + 1;

    // --- defaults (any order, any subset) ----------------------------------
    song->default_dur = 4;
    song->default_oct = 6;
    song->bpm = 63;

    for (;;) {
        skip_spaces(&p, end);
        if (p >= end) return false;          // missing second ':'
        if (*p == ':') { p++; break; }
        if (*p == ',') { p++; continue; }    // tolerate empty entries

        char key = to_lower(*p++);
        skip_spaces(&p, end);
        if (p >= end || *p != '=') return false;
        p++;
        skip_spaces(&p, end);
        uint32_t v;
        if (!parse_uint(&p, end, 4, &v)) return false;

        switch (key) {
            case 'd':
                if (!valid_duration(v)) return false;
                song->default_dur = (uint8_t)v;
                break;
            case 'o':
                if (v < RTTTL_MIN_OCTAVE || v > RTTTL_MAX_OCTAVE) return false;
                song->default_oct = (uint8_t)v;
                break;
            case 'b':
                if (v < RTTTL_MIN_BPM || v > RTTTL_MAX_BPM) return false;
                song->bpm = (uint16_t)v;
                break;
            default:
                return false;
        }
        skip_spaces(&p, end);
        if (p >= end) return false;
        if (*p == ',') { p++; continue; }
        if (*p == ':') { p++; break; }
        return false;
    }

    song->wholenote_ms = (60000u * 4u) / song->bpm;

    // --- notes: validate every token and accumulate the total -------------
    song->notes = p;
    song->notes_len = (size_t)(end - p);

    RtttlNote n;
    const char* np = song->notes;
    const char* nend = song->notes + song->notes_len;
    uint32_t total = 0;
    uint16_t count = 0;

    for (;;) {
        skip_spaces(&np, nend);
        if (np >= nend) break;
        if (*np == ',') { np++; continue; }
        if (!parse_note_token(&np, nend, song, &n)) return false;
        if (np < nend && *np == ',') np++;
        if (++count > RTTTL_MAX_NOTES) return false;
        total += n.duration_ms;
        if (total > RTTTL_MAX_TOTAL_MS) return false;
    }
    if (count == 0) return false;

    song->total_ms = total;
    song->note_count = count;
    return true;
}

void rtttl_iter_init(RtttlIter* it, const RtttlSong* song) {
    it->song = song;
    it->pos = 0;
}

bool rtttl_iter_next(RtttlIter* it, RtttlNote* out) {
    const RtttlSong* song = it->song;
    if (!song || !song->notes) return false;
    const char* p = song->notes + it->pos;
    const char* end = song->notes + song->notes_len;

    for (;;) {
        skip_spaces(&p, end);
        if (p >= end) { it->pos = song->notes_len; return false; }
        if (*p == ',') { p++; continue; }
        break;
    }
    if (!parse_note_token(&p, end, song, out)) {
        // Cannot happen for a song validated by rtttl_parse(); stop safely.
        it->pos = song->notes_len;
        return false;
    }
    if (p < end && *p == ',') p++;
    it->pos = (size_t)(p - song->notes);
    return true;
}
