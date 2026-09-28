#pragma once
// Bounds-checked RTTTL (Ring Tone Text Transfer Language) parser.
// Pure C++: no Arduino dependencies, so it can be unit-tested on the host.
//
// Format:  name:d=4,o=5,b=100:32p,16e6,16e6,16p,...
//   - name        : up to RTTTL_MAX_NAME_LEN chars (may be empty)
//   - defaults    : any order / any subset of d= (1,2,4,8,16,32), o= (4..7), b= (25..900)
//   - notes       : [duration][note][#][.][octave][.]  separated by ','
//                   note in a..g / p (pause); whitespace around tokens is ignored
//
// rtttl_parse() validates the whole string (including every note) and computes
// the total playback time; strings longer than RTTTL_MAX_LEN, with unknown
// tokens or exceeding RTTTL_MAX_TOTAL_MS are rejected.

#include <stdint.h>
#include <stddef.h>

#define RTTTL_MAX_LEN       1024   // maximum accepted string length (bytes)
#define RTTTL_MAX_NAME_LEN  32
#define RTTTL_MAX_NOTES     512
#define RTTTL_MAX_TOTAL_MS  15000  // hard cap on melody duration

struct RtttlSong {
    const char* notes;        // points into the original string (first note token)
    size_t      notes_len;    // number of bytes in the note section
    uint8_t     default_dur;  // 1,2,4,8,16,32
    uint8_t     default_oct;  // 4..7
    uint16_t    bpm;
    uint32_t    wholenote_ms;
    uint32_t    total_ms;     // sum of all note/pause durations
    uint16_t    note_count;
    char        name[RTTTL_MAX_NAME_LEN + 1];
};

struct RtttlNote {
    uint16_t freq_hz;      // 0 = pause
    uint16_t duration_ms;
};

struct RtttlIter {
    const RtttlSong* song;
    size_t pos;
};

// Quick structural check: does the string look like RTTTL (two ':' separators,
// printable ASCII only, length within limits)? Cheap pre-filter before parsing.
bool rtttl_looks_like(const char* s);

// Full validation. Returns false on any error; `song` is left in an unspecified
// state in that case. `s` must be NUL-terminated.
bool rtttl_parse(const char* s, RtttlSong* song);

// Iterate over the notes of an already validated song.
void rtttl_iter_init(RtttlIter* it, const RtttlSong* song);
bool rtttl_iter_next(RtttlIter* it, RtttlNote* out);
