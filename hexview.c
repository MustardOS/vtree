// hexview.c — hex viewer / editor for vTree
// -----------------------------------------------------------------------------
// Layout:
//
//   header:   filename | offset | short values
//   column:   OFFSET  00 01 02...0F  |  ASCII/unicode text (RIGHT-aligned)
//   footer:   mode hints
//
// The ASCII/text panel is pinned to the RIGHT edge of the screen so that on
// wider / higher-resolution displays the hex grid fills the left side and the
// text panel always reads cleanly at the right margin.  The hex grid itself is
// left-anchored from the offset column as before.
//
// Gamepad controls (READ mode):
//   D-pad Up/Dn        move cursor ±16 bytes (one row)
//   D-pad Left/Right   move cursor ±1 byte
//   L1 / R1            page up / page down (±16 rows = 256 bytes)
//   A                  enter EDIT mode
//   X                  enter GOTO mode
//   Start              save file in-place
//   B / Menu           close, return to explorer
//
// EDIT mode:
//   Up/Dn   +1 / -1 to edit buffer nibble-by-nibble
//   Left     shift buffer left (×16)
//   Right    shift buffer right (÷16)
//   A        write buffer byte at cursor, exit edit mode
//   B        cancel edit
//
// GOTO mode:
//   Up/Dn   +1 / -1 to target address
//   Left     ×16
//   Right    ÷16
//   A        jump to address
//   B        cancel
// -----------------------------------------------------------------------------

#include "vtree.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------
#define HV_COLS        16   // bytes per row
#define HV_MAX_SIZE   (16 * 1024 * 1024)  // 16 MB hard cap

// Right-side margin (pixels) between the last ASCII char and the screen edge
#define HV_ASCII_MARGIN 6
// Extra gap (pixels) between the offset label column and the first hex cell
#define HV_HEX_GAP 14

// ---------------------------------------------------------------------------
// Color coding
// ---------------------------------------------------------------------------
typedef enum {
    HC_ZERO, HC_CTRL, HC_SPACE, HC_PUNCT, HC_DIGIT,
    HC_LETTER, HC_HIGH, HC_FULL, HC_CURSOR, HC_COUNT
} HexColor;

static SDL_Color hv_color(HexColor c) {
    switch (c) {
    case HC_ZERO:   return cfg.theme.hex_zero;
    case HC_CTRL:   return cfg.theme.hex_ctrl;
    case HC_SPACE:  return cfg.theme.hex_space;
    case HC_PUNCT:  return cfg.theme.hex_punct;
    case HC_DIGIT:  return cfg.theme.hex_digit;
    case HC_LETTER: return cfg.theme.hex_letter;
    case HC_HIGH:   return cfg.theme.hex_high;
    case HC_FULL:   return cfg.theme.hex_full;
    case HC_CURSOR: { SDL_Color cur = cfg.theme.highlight_bg; cur.a=255; return cur; }
    default:        return cfg.theme.text;
    }
}

static HexColor hv_classify(unsigned char b) {
    if (b == 0x00)             return HC_ZERO;
    if (b <  0x20)             return HC_CTRL;
    if (b == 0x20)             return HC_SPACE;
    if (b <  0x30)             return HC_PUNCT;
    if (b <  0x3A)             return HC_DIGIT;
    if (b <  0x41)             return HC_PUNCT;
    if (b <  0x5B)             return HC_LETTER;
    if (b <  0x61)             return HC_PUNCT;
    if (b <  0x7B)             return HC_LETTER;
    if (b <  0x80)             return HC_PUNCT;
    if (b == 0xFF)             return HC_FULL;
    return HC_HIGH;
}

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
typedef enum { HVM_READ, HVM_EDIT, HVM_GOTO } HexViewMode;

typedef struct {
    char           path[MAX_PATH];
    unsigned char *data;
    size_t         size;
    bool           modified;
    bool           read_only;

    int            top_row;
    int            cursor;

    HexViewMode    mode;
    int            edit_buf;
    int            goto_addr;
} HexViewState;

static HexViewState hv;

// ---------------------------------------------------------------------------
// Auto-sized font — opened in hexview_open() to best fill the screen width.
// hv_font == NULL means font_hex (the global fallback) is used instead.
// ---------------------------------------------------------------------------
static TTF_Font *hv_font    = NULL;
static int       hv_font_pt = 0;

// Active hex font and its point size
static TTF_Font *hv_f(void)  { return hv_font  ? hv_font  : font_hex;          }
static int       hv_pt(void) { return hv_font_pt ? hv_font_pt : cfg.font_size_hex; }

// Total horizontal pixels required for one data row at the current font
static int hv_row_width(void) {
    int ow = 0, cw = 0, aw = 0;
    TTF_Font *f = hv_f();
    if (f) {
        TTF_SizeText(f, "000000 ", &ow, NULL);
        TTF_SizeText(f, "XX ",     &cw, NULL);
        TTF_SizeText(f, "W",       &aw, NULL);
    } else {
        int pt = hv_pt();
        ow = pt * 5; cw = pt * 2; aw = pt;
    }
    return ow + HV_HEX_GAP + HV_COLS * cw + 3 + HV_COLS * aw + HV_ASCII_MARGIN;
}

// Open the hex viewer at the largest font size that makes the row fit screen_w.
// Searches [6..72] pt regardless of cfg.font_size_hex (which is kept only as a
// fallback for cases where the font file cannot be opened).
static void hv_autofit(void) {
    if (hv_font) { TTF_CloseFont(hv_font); hv_font = NULL; hv_font_pt = 0; }
    if (!vtree_font_path[0]) return;

    int lo = 6, hi = 72, best_pt = -1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        TTF_Font *tf = TTF_OpenFont(vtree_font_path, mid);
        if (!tf) { hi = mid - 1; continue; }

        // Temporarily install the test font so hv_row_width() measures it
        hv_font = tf; hv_font_pt = mid;
        int total = hv_row_width();
        TTF_CloseFont(tf); hv_font = NULL; hv_font_pt = 0;

        if (total <= cfg.screen_w) { best_pt = mid; lo = mid + 1; }
        else                       { hi = mid - 1;                 }
    }

    if (best_pt > 0 && best_pt != cfg.font_size_hex) {
        hv_font    = TTF_OpenFont(vtree_font_path, best_pt);
        hv_font_pt = best_pt;   // NULL open silently falls back to font_hex
    }
    vtree_log("[hexview] auto-fit: configured=%dpt  best-fit=%dpt  using=%dpt\n",
              cfg.font_size_hex, best_pt > 0 ? best_pt : cfg.font_size_hex,
              hv_font ? hv_font_pt : cfg.font_size_hex);
}

// ---------------------------------------------------------------------------
// Geometry helpers — all derived from hv_f() metrics
// ---------------------------------------------------------------------------

// Row heights for the two header sub-strips
static int hv_info_h(void)  { return cfg.font_size_header + 4 + 4 + 4; }
static int hv_clab_h(void)  { return cfg.font_size_footer + 6;          }
static int hv_head_h(void)  { return hv_info_h() + hv_clab_h();         }
static int hv_foot_h(void)  { return cfg.font_size_footer + 16;         }
static int hv_row_h(void)   { return (hv_f() ? TTF_FontHeight(hv_f()) : hv_pt()) + 4; }

static int hv_visible(void) {
    return (cfg.screen_h - hv_head_h() - hv_foot_h()) / hv_row_h();
}

// Width of "XX " in the active hex font
static int hv_cell_w(void) {
    if (!hv_f()) return hv_pt() * 2;
    int w = 0; TTF_SizeText(hv_f(), "XX ", &w, NULL);
    return w;
}

// Width of "000000 " (offset label)
static int hv_off_w(void) {
    if (!hv_f()) return hv_pt() * 5;
    int w = 0; TTF_SizeText(hv_f(), "000000 ", &w, NULL);
    return w;
}

// Width of one ASCII glyph cell — measured with "W" (widest typical char)
static int hv_ascii_cw(void) {
    if (!hv_f()) return hv_pt();
    int w = 0; TTF_SizeText(hv_f(), "W", &w, NULL);
    return w;
}

// X where hex data starts — extra gap after the OFFSET label for visual breathing room
static int hv_hex_x(void) { return hv_off_w() + HV_HEX_GAP; }

// ── ASCII panel geometry — RIGHT-anchored ────────────────────────────────────
// The text panel sits flush against the right edge with a small margin.
// ascii_x is computed so that (ascii_x + HV_COLS*acw + HV_ASCII_MARGIN) == screen_w.
static int hv_ascii_x(void) {
    return cfg.screen_w - HV_COLS * hv_ascii_cw() - HV_ASCII_MARGIN;
}

// Separator line between hex grid and ASCII panel: sits just left of ascii_x
static int hv_sep_x(void) { return hv_ascii_x() - 3; }

static void hv_clamp_scroll(void) {
    int vis = hv_visible();
    int cur_row = hv.cursor / HV_COLS;
    if (cur_row < hv.top_row)
        hv.top_row = cur_row;
    if (cur_row >= hv.top_row + vis)
        hv.top_row = cur_row - vis + 1;
    if (hv.top_row < 0) hv.top_row = 0;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
void hexview_open(const char *path) {
    if (hv.data) { free(hv.data); hv.data = NULL; }
    memset(&hv, 0, sizeof(hv));
    strncpy(hv.path, path, MAX_PATH - 1);
    hv.mode     = HVM_READ;
    hv.top_row  = 0;
    hv.cursor   = 0;

    FILE *f = fopen(path, "rb");
    if (!f) {
        vtree_log("[hexview] open FAILED: %s (errno %d: %s)\n", path, errno, strerror(errno));
        hv.read_only = true;
        current_mode = MODE_VIEW_HEX;
        return;
    }
    fseek(f, 0, SEEK_END);
    long fsz = ftell(f);
    rewind(f);
    if (fsz > HV_MAX_SIZE) fsz = HV_MAX_SIZE;

    hv.data = malloc(fsz + 1);
    if (!hv.data) { fclose(f); current_mode = MODE_VIEW_HEX; return; }
    hv.size = fread(hv.data, 1, fsz, f);
    fclose(f);

    hv.read_only = (access(path, W_OK) != 0);

    vtree_log("[hexview] opened: %s  size=%zu bytes%s\n",
              path, hv.size, hv.read_only ? "  [read-only]" : "");

    hv_autofit();   // choose the largest font size that fills screen width

    current_mode = MODE_VIEW_HEX;
}

void hexview_close(void) {
    if (hv.data) { free(hv.data); hv.data = NULL; }
    hv.size = 0;
    if (hv_font) { TTF_CloseFont(hv_font); hv_font = NULL; hv_font_pt = 0; }
}

static bool hv_save(void) {
    FILE *f = fopen(hv.path, "wb");
    if (!f) {
        vtree_log("[hexview] save FAILED: %s (errno %d: %s)\n", hv.path, errno, strerror(errno));
        return false;
    }
    fwrite(hv.data, 1, hv.size, f);
    fclose(f);
    vtree_log("[hexview] saved: %s  (%zu bytes)\n", hv.path, hv.size);
    return true;
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------
void hexview_handle_button(SDL_GameControllerButton btn,
                           bool *dpad_up_held, bool *dpad_down_held,
                           bool *dpad_left_held, bool *dpad_right_held,
                           Uint32 now) {
    (void)dpad_up_held; (void)dpad_down_held;
    (void)dpad_left_held; (void)dpad_right_held;
    (void)now;
    int sz = (int)hv.size;
    if (sz < 1) sz = 1;

    if (hv.mode == HVM_READ) {
        if (btn == cfg.k_back || btn == cfg.k_menu) {
            current_mode = MODE_EXPLORER; return;
        }
        if (btn == SDL_CONTROLLER_BUTTON_START) {
            if (hv.modified && !hv.read_only) { hv_save(); hv.modified = false; } return;
        }
        if (btn == cfg.k_confirm && !hv.read_only) {
            hv.mode = HVM_EDIT; hv.edit_buf = hv.data ? (int)hv.data[hv.cursor] : 0; return;
        }
        if (btn == cfg.k_mark) {
            hv.mode = HVM_GOTO; hv.goto_addr = hv.cursor; return;
        }
        if (btn == SDL_CONTROLLER_BUTTON_DPAD_UP) {
            hv.cursor -= HV_COLS; if (hv.cursor < 0) hv.cursor = 0;
            hv_clamp_scroll(); return;
        }
        if (btn == SDL_CONTROLLER_BUTTON_DPAD_DOWN) {
            hv.cursor += HV_COLS; if (hv.cursor >= sz) hv.cursor = sz - 1;
            hv_clamp_scroll(); return;
        }
        if (btn == SDL_CONTROLLER_BUTTON_DPAD_LEFT) {
            hv.cursor--; if (hv.cursor < 0) hv.cursor = 0;
            hv_clamp_scroll(); return;
        }
        if (btn == SDL_CONTROLLER_BUTTON_DPAD_RIGHT) {
            hv.cursor++; if (hv.cursor >= sz) hv.cursor = sz - 1;
            hv_clamp_scroll(); return;
        }
        if (btn == cfg.k_pgup) {
            hv.cursor -= HV_COLS * hv_visible(); if (hv.cursor < 0) hv.cursor = 0;
            hv_clamp_scroll(); return;
        }
        if (btn == cfg.k_pgdn) {
            hv.cursor += HV_COLS * hv_visible(); if (hv.cursor >= sz) hv.cursor = sz - 1;
            hv_clamp_scroll(); return;
        }
    }
    else if (hv.mode == HVM_EDIT) {
        if (btn == cfg.k_back) { hv.mode = HVM_READ; return; }
        if (btn == cfg.k_confirm) {
            if (hv.data) hv.data[hv.cursor] = (unsigned char)hv.edit_buf;
            hv.modified = true; hv.mode = HVM_READ; return;
        }
        if (btn == SDL_CONTROLLER_BUTTON_DPAD_UP)    { hv.edit_buf++; if (hv.edit_buf > 0xFF) hv.edit_buf = 0xFF; return; }
        if (btn == SDL_CONTROLLER_BUTTON_DPAD_DOWN)  { hv.edit_buf--; if (hv.edit_buf < 0)    hv.edit_buf = 0;    return; }
        if (btn == SDL_CONTROLLER_BUTTON_DPAD_LEFT)  { hv.edit_buf = SDL_min(hv.edit_buf * 16, 0xFF); return; }
        if (btn == SDL_CONTROLLER_BUTTON_DPAD_RIGHT) { hv.edit_buf = hv.edit_buf / 16; return; }
    }
    else if (hv.mode == HVM_GOTO) {
        if (btn == cfg.k_back) { hv.mode = HVM_READ; return; }
        if (btn == cfg.k_confirm) {
            hv.cursor = SDL_clamp(hv.goto_addr, 0, sz - 1);
            hv_clamp_scroll(); hv.mode = HVM_READ; return;
        }
        if (btn == SDL_CONTROLLER_BUTTON_DPAD_UP)    { hv.goto_addr++; if (hv.goto_addr >= sz) hv.goto_addr = sz - 1; return; }
        if (btn == SDL_CONTROLLER_BUTTON_DPAD_DOWN)  { hv.goto_addr--; if (hv.goto_addr < 0)   hv.goto_addr = 0;      return; }
        if (btn == SDL_CONTROLLER_BUTTON_DPAD_LEFT)  { hv.goto_addr = SDL_min((int)((unsigned)hv.goto_addr * 16), sz - 1); return; }
        if (btn == SDL_CONTROLLER_BUTTON_DPAD_RIGHT) { hv.goto_addr = hv.goto_addr / 16; if (hv.goto_addr < 0) hv.goto_addr = 0; return; }
    }
}

void hexview_handle_repeat(Uint32 now) { (void)now; }

// ---------------------------------------------------------------------------
// Render helpers
// ---------------------------------------------------------------------------
static void hv_draw_ascii(unsigned char b, int x, int y, SDL_Color col) {
    char buf[8];
    if (b > 0x20 && b < 0x7E) {
        buf[0] = (char)b; buf[1] = '\0';
    } else if (b > 0x80) {
        buf[0] = (char)(0xC0 + b / 0x40);
        buf[1] = (char)(0x80 + b % 0x40);
        buf[2] = '\0';
    } else if (b == 0x00) {
        buf[0] = '0'; buf[1] = '\0';
    } else if (b == 0x20) {
        buf[0] = (char)0xC2; buf[1] = (char)0xB7; buf[2] = '\0'; // U+00B7 middle dot
    } else {
        buf[0] = '.'; buf[1] = '\0';
    }
    draw_txt(hv_f(), buf, x, y, col);
}

// ---------------------------------------------------------------------------
// Render
// ---------------------------------------------------------------------------
void hexview_draw(void) {
    SDL_SetRenderDrawColor(renderer,
        cfg.theme.bg.r, cfg.theme.bg.g, cfg.theme.bg.b, 255);
    SDL_RenderClear(renderer);

    int info_h  = hv_info_h();
    int clab_h  = hv_clab_h();
    int head_h  = hv_head_h();
    int foot_h  = hv_foot_h();
    int row_h   = hv_row_h();
    int vis     = hv_visible();
    int hex_x   = hv_hex_x();
    int cell_w  = hv_cell_w();
    int ascii_x = hv_ascii_x();
    int sep_x   = hv_sep_x();
    int acw     = hv_ascii_cw();

    // ── Header info row ──────────────────────────────────────────────────────
    SDL_SetRenderDrawColor(renderer,
        cfg.theme.header_bg.r, cfg.theme.header_bg.g, cfg.theme.header_bg.b, 255);
    SDL_Rect hr = {0, 0, cfg.screen_w, info_h};
    SDL_RenderFillRect(renderer, &hr);

    // Vertically centre text but bias it upward a few px so it clears the
    // divider line at the bottom of the info row comfortably.
    int info_text_y = (info_h - cfg.font_size_header) / 2 - 2;

    const char *bname = strrchr(hv.path, '/');
    bname = bname ? bname + 1 : hv.path;
    char fname_part[MAX_PATH + 16];
    snprintf(fname_part, sizeof(fname_part), "%s%s%s",
             bname,
             hv.modified  ? " [mod]" : "",
             hv.read_only ? " [R/O]" : "");
    draw_txt(font_header, fname_part, 6, info_text_y, cfg.theme.text);

    if (hv.data && hv.size > 0 && hv.cursor < (int)hv.size) {
        unsigned char  cb = hv.data[hv.cursor];
        signed char    sb = (signed char)cb;
        unsigned short us = 0; short ss = 0;
        if (hv.cursor + 1 < (int)hv.size) {
            us = (unsigned short)(cb | ((unsigned short)hv.data[hv.cursor+1] << 8));
            ss = (short)us;
        }
        char vals[80];
        snprintf(vals, sizeof(vals), "0x%06X  u8:%u s8:%d  u16:%u s16:%d",
                 hv.cursor, cb, sb, us, ss);
        int vw = 0;
        if (font_header) TTF_SizeText(font_header, vals, &vw, NULL);
        draw_txt(font_header, vals, cfg.screen_w - vw - 6, info_text_y, cfg.theme.marked);
    }

    // ── Column-label strip ───────────────────────────────────────────────────
    int cy = info_h;
    SDL_SetRenderDrawColor(renderer,
        cfg.theme.alt_bg.r, cfg.theme.alt_bg.g, cfg.theme.alt_bg.b, 255);
    SDL_Rect clr = {0, cy, cfg.screen_w, clab_h};
    SDL_RenderFillRect(renderer, &clr);

    // OFFSET label — left-aligned with a fixed margin so it never touches
    // the separator line to its right.
#define HV_OFFSET_MARGIN 6
    draw_txt(font_footer, "OFFSET", HV_OFFSET_MARGIN,
             cy + (clab_h - cfg.font_size_footer) / 2,
             cfg.theme.text_disabled);

    // Separator after OFFSET label
    SDL_SetRenderDrawColor(renderer,
        cfg.theme.text_disabled.r, cfg.theme.text_disabled.g,
        cfg.theme.text_disabled.b, 150);
    SDL_RenderDrawLine(renderer, hex_x - 2, cy, hex_x - 2, cy + clab_h);

    // Column labels "00".."0F" — spaced to match cell_w
    for (int c = 0; c < HV_COLS; c++) {
        char lbl[4]; snprintf(lbl, sizeof(lbl), "%02X", c);
        draw_txt(font_footer, lbl, hex_x + c * cell_w,
                 cy + (clab_h - cfg.font_size_footer) / 2,
                 cfg.theme.text_disabled);
    }

    // "TEXT" label right-aligned in the ASCII panel
    {
        const char *txt_lbl = "TEXT";
        int tw = 0;
        if (font_footer) TTF_SizeText(font_footer, txt_lbl, &tw, NULL);
        // Right-align: place so its right edge lines up with the panel's right edge
        int label_x = cfg.screen_w - HV_ASCII_MARGIN - tw;
        if (label_x > sep_x + 4)
            draw_txt(font_footer, txt_lbl, label_x,
                     cy + (clab_h - cfg.font_size_footer) / 2, cfg.theme.text_disabled);
    }

    // Separator between hex grid and ASCII panel — in the column-label strip too
    SDL_SetRenderDrawColor(renderer,
        cfg.theme.text_disabled.r, cfg.theme.text_disabled.g,
        cfg.theme.text_disabled.b, 150);
    SDL_RenderDrawLine(renderer, sep_x, cy, sep_x, cy + clab_h);

    // Bottom border of header
    SDL_SetRenderDrawColor(renderer,
        cfg.theme.menu_border.r, cfg.theme.menu_border.g,
        cfg.theme.menu_border.b, 255);
    SDL_RenderDrawLine(renderer, 0, head_h - 1, cfg.screen_w, head_h - 1);

    // ── Data rows ─────────────────────────────────────────────────────────────
    for (int r = 0; r < vis; r++) {
        int row  = hv.top_row + r;
        int base = row * HV_COLS;
        if ((size_t)base >= hv.size && hv.size > 0) break;
        int ry = head_h + r * row_h;

        // Alternating row tint
        if (r % 2 != 0) {
            SDL_SetRenderDrawColor(renderer,
                cfg.theme.alt_bg.r, cfg.theme.alt_bg.g, cfg.theme.alt_bg.b, 255);
            SDL_Rect ar = {0, ry, cfg.screen_w, row_h};
            SDL_RenderFillRect(renderer, &ar);
        }

        // Offset label — left aligned, uses hex font
        char off_lbl[16]; snprintf(off_lbl, sizeof(off_lbl), "%06X", base);
        draw_txt(hv_f(), off_lbl, 4, ry + 2, cfg.theme.marked);

        // Separator between offset and hex columns
        SDL_SetRenderDrawColor(renderer,
            cfg.theme.text_disabled.r, cfg.theme.text_disabled.g,
            cfg.theme.text_disabled.b, 80);
        SDL_RenderDrawLine(renderer, hex_x - 2, ry, hex_x - 2, ry + row_h);

        // Separator between hex grid and ASCII panel
        SDL_SetRenderDrawColor(renderer,
            cfg.theme.text_disabled.r, cfg.theme.text_disabled.g,
            cfg.theme.text_disabled.b, 80);
        SDL_RenderDrawLine(renderer, sep_x, ry, sep_x, ry + row_h);

        // Hex bytes + ASCII panel
        for (int c = 0; c < HV_COLS; c++) {
            int idx = base + c;
            if ((size_t)idx >= hv.size) break;
            unsigned char b = hv.data[idx];
            bool is_cursor = (idx == hv.cursor);

            // ── Hex cell ────────────────────────────────────────────────────
            if (is_cursor) {
                SDL_SetRenderDrawColor(renderer,
                    cfg.theme.highlight_bg.r, cfg.theme.highlight_bg.g,
                    cfg.theme.highlight_bg.b, 255);
                SDL_Rect ch = {hex_x + c * cell_w, ry, cell_w - 1, row_h};
                SDL_RenderFillRect(renderer, &ch);
            }
            char hex[4];
            if (is_cursor && hv.mode == HVM_EDIT)
                snprintf(hex, sizeof(hex), "%02X", hv.edit_buf);
            else
                snprintf(hex, sizeof(hex), "%02X", b);
            SDL_Color hcol = is_cursor ? cfg.theme.marked : hv_color(hv_classify(b));
            draw_txt(hv_f(), hex, hex_x + c * cell_w, ry + 2, hcol);

            // ── ASCII cell — right-aligned panel ────────────────────────────
            int ax = ascii_x + c * acw;
            if (ax + acw <= cfg.screen_w) {
                if (is_cursor) {
                    SDL_SetRenderDrawColor(renderer,
                        cfg.theme.highlight_bg.r, cfg.theme.highlight_bg.g,
                        cfg.theme.highlight_bg.b, 255);
                    SDL_Rect ac = {ax, ry, acw, row_h};
                    SDL_RenderFillRect(renderer, &ac);
                }
                hv_draw_ascii(b, ax, ry + 2,
                              is_cursor ? cfg.theme.marked : hv_color(hv_classify(b)));
            }
        }
    }

    // ── Footer ────────────────────────────────────────────────────────────────
    SDL_SetRenderDrawColor(renderer,
        cfg.theme.header_bg.r, cfg.theme.header_bg.g, cfg.theme.header_bg.b, 255);
    SDL_Rect fr = {0, cfg.screen_h - foot_h, cfg.screen_w, foot_h};
    SDL_RenderFillRect(renderer, &fr);

    char hint[256] = "";
    if (hv.mode == HVM_READ) {
        if (!hv.read_only)
            snprintf(hint, sizeof(hint),
                     "D-Pad: Cursor   L1/R1: Page   A: Edit   X: Goto   Start: Save   B: Close");
        else
            snprintf(hint, sizeof(hint),
                     "D-Pad: Cursor   L1/R1: Page   X: Goto   B: Close   [Read-Only]");
    } else if (hv.mode == HVM_EDIT) {
        snprintf(hint, sizeof(hint),
                 "Edit: 0x%02X   Up/Dn: +/-1   L/R: x/16   A: Write   B: Cancel",
                 hv.edit_buf);
    } else if (hv.mode == HVM_GOTO) {
        snprintf(hint, sizeof(hint),
                 "Goto: 0x%06X   Up/Dn: +/-1   L/R: x/16   A: Jump   B: Cancel",
                 hv.goto_addr);
    }
    draw_txt(font_footer, hint, 8,
             cfg.screen_h - foot_h + (foot_h - cfg.font_size_footer) / 2,
             cfg.theme.text_disabled);
}
