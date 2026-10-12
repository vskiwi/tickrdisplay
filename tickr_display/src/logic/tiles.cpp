#include "tiles.h"
#include <string.h>
#include <stdio.h>

// Capitals / digits of FreeSansBold 9 / 12 / 18 pt are 13 / 18 / 26 px tall
// (hal_display measures the real bounds; these decide which steps a band
// may try at all).
const uint8_t tile_step_h[TILE_F_COUNT] = { 52, 36, 26, 18, 13 };

#define PAD_Y        2    // rows above / below the content of a cell
#define ROW_GAP      4    // between the bands of the stack look
#define BOTTOM_H     20   // the stack's bottom row: value 12 pt, change 12 pt
#define BOTTOM_H_CR  13   // ... in a cramped cell: 9 pt only (its capitals are 13 px)
#define TALL_H       96   // a cell of this height or more stacks three bands
#define CRAMPED_H    60   // below this the stack's bottom row is 9 pt
#define BAND_MARGIN     4 // white rows between the change band of a tall cell and the status band
#define BAND_MARGIN_CR  2 // ... between the bottom row of a cramped cell and the status band
#define TITLE_PCT       45  // the title band's share of a tall cell
#define TITLE_PCT_BANDED 40 // ... when the frame carries the status band (the rest keeps 18 pt rows)

static const char* const kLayoutNames[TILE_LAYOUT_COUNT] = { "single", "1x1", "1x2", "2x1", "2x2" };
static const char* const kLookNames[TILE_LOOK_COUNT]     = { "stack", "row", "ticker" };
static const char* const kSizeNames[TILE_SIZE_COUNT]     = { "auto", "l", "m", "s" };

static bool parse_name(const char* const* names, uint8_t n, const char* s, uint8_t* out) {
    if (!s) return false;
    for (uint8_t i = 0; i < n; i++) {
        if (strcmp(names[i], s) == 0) { *out = i; return true; }
    }
    return false;
}

const char* tile_layout_str(uint8_t layout) { return layout < TILE_LAYOUT_COUNT ? kLayoutNames[layout] : "single"; }
bool        tile_layout_parse(const char* s, uint8_t* out) { return parse_name(kLayoutNames, TILE_LAYOUT_COUNT, s, out); }
const char* tile_look_str(uint8_t look)     { return look < TILE_LOOK_COUNT ? kLookNames[look] : "stack"; }
bool        tile_look_parse(const char* s, uint8_t* out)   { return parse_name(kLookNames, TILE_LOOK_COUNT, s, out); }
const char* tile_size_str(uint8_t size)     { return size < TILE_SIZE_COUNT ? kSizeNames[size] : "auto"; }
bool        tile_size_parse(const char* s, uint8_t* out)   { return parse_name(kSizeNames, TILE_SIZE_COUNT, s, out); }

uint8_t tiles_layout_cells(uint8_t layout) {
    switch (layout) {
        case TILE_LAYOUT_1X1: return 1;
        case TILE_LAYOUT_1X2: return 2;
        case TILE_LAYOUT_2X1: return 2;
        case TILE_LAYOUT_2X2: return 4;
        default:              return 0;
    }
}

uint8_t tiles_layout_default(uint8_t tiles_n) {
    if (tiles_n == 1) return TILE_LAYOUT_1X1;
    if (tiles_n == 2) return TILE_LAYOUT_1X2;
    return TILE_LAYOUT_2X2;
}

bool tiles_cell_rect(uint8_t layout, uint8_t idx, TileRect* out) {
    if (idx >= tiles_layout_cells(layout)) return false;
    const bool two_cols = layout == TILE_LAYOUT_1X2 || layout == TILE_LAYOUT_2X2;
    const bool two_rows = layout == TILE_LAYOUT_2X1 || layout == TILE_LAYOUT_2X2;
    const uint8_t col = two_cols ? (idx & 1) : 0;
    const uint8_t row = two_cols ? (idx >> 1) : idx;
    // separators at x = 148 and y = 64 (the ticker grid's), the right / lower
    // cells start one pixel later and take the rest
    out->x = col ? GRID_CELL_W + 1 : 0;
    out->w = two_cols ? (col ? TILES_SCREEN_W - GRID_CELL_W - 1 : GRID_CELL_W) : TILES_SCREEN_W;
    out->y = row ? GRID_CELL_H + 1 : 0;
    out->h = two_rows ? (row ? TILES_SCREEN_H - GRID_CELL_H - 1 : GRID_CELL_H) : TILES_SCREEN_H;
    return true;
}

uint8_t tiles_status_cell(uint8_t layout) {
    uint8_t n = tiles_layout_cells(layout);
    return n ? (uint8_t)(n - 1) : 0;
}

void tiles_usable(uint8_t layout, uint8_t idx, bool status, TileRect* out) {
    if (!tiles_cell_rect(layout, idx, out)) { out->x = out->y = out->w = out->h = 0; return; }
    if (status && idx == tiles_status_cell(layout)) out->h -= TILES_STATUS_H;
}

bool tiles_cramped(const TileRect* usable) {
    return usable->h < CRAMPED_H;
}

int16_t tiles_frame_min_h(uint8_t layout, bool status) {
    TileRect u;
    tiles_usable(layout, tiles_status_cell(layout), status, &u);
    return u.h;
}

void tiles_geometry(uint8_t look, const TileRect* usable, int16_t frame_h, bool status, TileGeom* out) {
    memset(out, 0, sizeof(*out));
    out->cramped = tiles_cramped(usable);
    // The cell that carries the status band (cramped, or every tall cell of a
    // frame with the band - their geometry is uniform) keeps a margin above it.
    const bool banded = status && (out->cramped || usable->h >= TALL_H);
    const int16_t H = usable->h - 2 * PAD_Y - (banded && out->cramped ? BAND_MARGIN_CR : 0);
    if (look != TILE_LOOK_STACK) {
        out->rows = 1;
        out->title_top = out->value_top = out->change_top = PAD_Y;
        out->title_h = out->value_h = out->change_h = H;
        return;
    }
    if (usable->h >= TALL_H) {
        // One geometry for every tall cell of the frame, from its smallest cell
        // (the rows of a 1x2 align; a taller cell leaves its extra rows white).
        // With the band: a narrower title band and a margin above the badges,
        // so the value and the change keep 18 pt (26 px) and stay clear of it.
        const int16_t ref = (frame_h > 0 && frame_h < usable->h ? frame_h : usable->h) - 2 * PAD_Y;
        out->rows = 3;
        out->title_top = PAD_Y;
        out->title_h = (int16_t)(ref * (banded ? TITLE_PCT_BANDED : TITLE_PCT) / 100);
        const int16_t rest = ref - out->title_h - 2 * ROW_GAP - (banded ? BAND_MARGIN : 0);
        out->value_top = out->title_top + out->title_h + ROW_GAP;
        out->value_h = rest / 2;
        out->change_top = out->value_top + out->value_h + ROW_GAP;
        out->change_h = rest - out->value_h;
        return;
    }
    out->rows = 2;
    const int16_t bottom = out->cramped ? BOTTOM_H_CR : BOTTOM_H;
    out->title_top = PAD_Y;
    out->title_h = H - bottom - PAD_Y;
    out->value_top = out->change_top = out->title_top + out->title_h + PAD_Y;
    out->value_h = out->change_h = bottom;
}

// Steps from `first` down to TILE_F9 whose height fits `band_h`; TILE_F9 always closes the list.
static uint8_t ladder(uint8_t first, int16_t band_h, uint8_t* out) {
    uint8_t n = 0;
    for (uint8_t s = first; s < TILE_F9; s++) {
        if (tile_step_h[s] <= band_h) out[n++] = s;
    }
    out[n++] = TILE_F9;
    return n;
}

void tiles_steps(uint8_t look, uint8_t size, const TileGeom* g, TileSteps* out) {
    uint8_t first = size == TILE_SIZE_M ? TILE_F18 : size == TILE_SIZE_S ? TILE_F12 : TILE_F18X2;
    // one line shares its width three ways: the title never doubles 18 pt there
    if (look == TILE_LOOK_ROW && first < TILE_F12X2) first = TILE_F12X2;
    uint8_t smaller = first < TILE_F9 ? (uint8_t)(first + 1) : (uint8_t)TILE_F9;
    out->title_n  = ladder(first, g->title_h, out->title);
    out->value_n  = ladder(smaller, g->value_h, out->value);
    out->change_n = ladder(smaller, g->change_h, out->change);
}

bool tiles_fit_row(TilesMeasureFn measure, void* ctx, const char* title, const char* value,
                   const char* change, const TileSteps* s, int16_t max_w, int16_t gap, int16_t extra,
                   uint8_t* ti, uint8_t* vi, uint8_t* ci) {
    *ti = *vi = *ci = 0;
    const bool has_t = title && title[0], has_v = value && value[0], has_c = change && change[0];
    const int16_t parts = (int16_t)has_t + (int16_t)has_v + (int16_t)has_c;
    const int16_t fixed = (parts > 1 ? (parts - 1) * gap : 0) + (has_c ? extra : 0);
    for (;;) {
        int16_t w = fixed;
        if (has_t) w += measure(title,  s->title[*ti],  ctx);
        if (has_v) w += measure(value,  s->value[*vi],  ctx);
        if (has_c) w += measure(change, s->change[*ci], ctx);
        if (w <= max_w) return true;
        if (has_c && *ci + 1 < s->change_n)      (*ci)++;
        else if (has_v && *vi + 1 < s->value_n)  (*vi)++;
        else if (has_t && *ti + 1 < s->title_n)  (*ti)++;
        else return false;
    }
}

void tiles_style_resolve(const TileStyleSpec* root, const TileStyleSpec* tile, TileStyle* out) {
    out->look = TILE_LOOK_STACK;
    out->size = TILE_SIZE_AUTO;
    out->invert = false;
    const TileStyleSpec* layers[2] = { root, tile };
    for (int i = 0; i < 2; i++) {
        const TileStyleSpec* l = layers[i];
        if (!l) continue;
        if (l->has_look)   out->look = l->st.look;
        if (l->has_size)   out->size = l->st.size;
        if (l->has_invert) out->invert = l->st.invert;
    }
}

static uint32_t fnv_byte(uint32_t h, uint8_t b) { h ^= b; return h * 16777619u; }
static uint32_t fnv_str(uint32_t h, const char* s) {
    for (; *s; s++) h = fnv_byte(h, (uint8_t)*s);
    return fnv_byte(h, 0xFF);       // terminator: "AB","C" differs from "A","BC"
}

uint32_t tiles_frame_key(const TilesFrame* f) {
    uint32_t h = 2166136261u ^ 0x54494C45u;    // 'TILE': apart from grid_set_key's seed
    h = fnv_byte(h, f->layout);
    h = fnv_byte(h, f->status ? 1 : 0);
    for (uint8_t i = 0; i < TILES_MAX && i < f->n; i++) {
        const Tile& t = f->t[i];
        h = fnv_byte(h, t.has ? 1 : 0);
        if (!t.has) continue;
        h = fnv_byte(h, t.style.invert ? 1 : 0);
        h = fnv_byte(h, t.style.look);
        h = fnv_byte(h, t.style.size);
        h = fnv_str(h, t.title);
    }
    return h;
}

void tiles_join_titles(const TilesFrame* f, char* buf, size_t cap) {
    if (!cap) return;
    buf[0] = '\0';
    size_t len = 0;
    for (uint8_t i = 0; i < TILES_MAX && i < f->n; i++) {
        if (!f->t[i].has || !f->t[i].title[0]) continue;
        int w = snprintf(buf + len, cap - len, "%s%s", len ? ", " : "", f->t[i].title);
        if (w < 0 || (size_t)w >= cap - len) { buf[cap - 1] = '\0'; return; }
        len += (size_t)w;
    }
}
