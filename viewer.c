// viewer.c — text file viewer / editor for vTree
// -----------------------------------------------------------------------------
// Layout:
//   header bar  (filename + modified flag)
//   line area   (monospaced lines, scrollable)
//   footer bar  (line:col info + key hints)
//
// Gamepad controls (read mode):
//   D-pad Up/Dn       scroll one line
//   L1/R1             page up / page down
//   A                 enter edit mode (opens OSK for current line)
//   B / Menu          close viewer, return to explorer
//   Start             save file in-place (only if modified)
//
// Edit mode: the existing OSK opens pre-filled with the current line's text.
// On OSK confirm the edited line is spliced back into the buffer.
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
#define TV_MAX_LINES   4096
#define TV_MAX_LINE_LEN 1024

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
typedef struct {
    char   path[MAX_PATH];
    char   lines[TV_MAX_LINES][TV_MAX_LINE_LEN]; // line storage
    int    line_count;
    int    top_line;      // first visible line index
    int    cur_line;      // cursor line index
    int    x_off;         // horizontal pixel scroll offset (read mode only)
    bool   modified;
    bool   read_only;     // set if file couldn't be opened for writing
} TextViewState;

static TextViewState tv;

// Forward-declared helpers from main.c OSK system (accessed via vtree.h extern)
// The OSK is controlled by setting current_mode = MODE_OSK and calling
// osk_enter_tv() — a new entry point we add in main.c that feeds back the
// edited line to viewer_osk_commit().
// We use a callback approach via a global:
static bool   tv_osk_pending = false;   // OSK was opened for a line edit
static int    tv_osk_line    = -1;      // which line is being edited

// Line-ops menu state
static bool tv_menu_open   = false;
static int  tv_menu_sel    = 0;         // 0=New Line, 1=Duplicate, 2=Delete
static bool tv_confirm_del = false;     // waiting for yes/no on non-empty line delete

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Actual row height — uses TTF metrics so descenders (g,y,p…) never clip
static int tv_item_h(void) {
    return (font_list ? TTF_FontHeight(font_list) : cfg.font_size_list) + 4;
}

// Count visible lines that fit on screen
static int tv_visible_lines(void) {
    int head_h = cfg.font_size_header + 12;
    int foot_h = cfg.font_size_footer + 16;
    return (cfg.screen_h - head_h - foot_h) / tv_item_h();
}

// Clamp scroll so cursor is always visible
static void tv_clamp_scroll(void) {
    int vis = tv_visible_lines();
    if (tv.cur_line < tv.top_line)
        tv.top_line = tv.cur_line;
    if (tv.cur_line >= tv.top_line + vis)
        tv.top_line = tv.cur_line - vis + 1;
    if (tv.top_line < 0) tv.top_line = 0;
    if (tv.top_line > tv.line_count - 1)
        tv.top_line = SDL_max(0, tv.line_count - 1);
}

// Split raw file data into lines (handles \r\n and \n)
static void tv_split_lines(const char *data, size_t len) {
    tv.line_count = 0;
    int lstart = 0;
    for (int i = 0; i <= (int)len; i++) {
        bool eol = (i == (int)len) || (data[i] == '\n');
        if (eol) {
            int llen = i - lstart;
            if (llen > 0 && data[lstart + llen - 1] == '\r') llen--; // strip \r
            if (llen > TV_MAX_LINE_LEN - 1) llen = TV_MAX_LINE_LEN - 1;
            memcpy(tv.lines[tv.line_count], data + lstart, llen);
            tv.lines[tv.line_count][llen] = '\0';
            tv.line_count++;
            lstart = i + 1;
            if (tv.line_count >= TV_MAX_LINES) break;
        }
    }
    if (tv.line_count == 0) {
        tv.lines[0][0] = '\0';
        tv.line_count  = 1;
    }
}

// Rebuild raw text from lines and write to disk
static bool tv_save(void) {
    FILE *f = fopen(tv.path, "w");
    if (!f) {
        vtree_log("[viewer] save FAILED: %s (errno %d: %s)\n", tv.path, errno, strerror(errno));
        return false;
    }
    for (int i = 0; i < tv.line_count; i++) {
        fputs(tv.lines[i], f);
        if (i < tv.line_count - 1) fputc('\n', f);
    }
    fclose(f);
    vtree_log("[viewer] saved: %s  (%d lines)\n", tv.path, tv.line_count);
    return true;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void viewer_open(const char *path) {
    memset(&tv, 0, sizeof(tv));
    strncpy(tv.path, path, MAX_PATH - 1);
    tv.top_line   = 0;
    tv.cur_line   = 0;
    tv.modified   = false;
    tv_osk_pending = false;
    tv_osk_line   = -1;
    tv_menu_open   = false;
    tv_confirm_del = false;

    // Read file via standard I/O (no LOVE2D dependency)
    FILE *f = fopen(path, "rb");
    if (!f) {
        vtree_log("[viewer] open FAILED: %s (errno %d: %s)\n", path, errno, strerror(errno));
        strncpy(tv.lines[0], "[could not open file]", TV_MAX_LINE_LEN - 1);
        tv.line_count = 1;
        tv.read_only  = true;
        current_mode  = MODE_VIEW_TEXT;
        return;
    }
    fseek(f, 0, SEEK_END);
    long fsz = ftell(f);
    rewind(f);

    // Clamp to a sane limit — we're not a full editor for 500 MB logs
    if (fsz > 1024 * 1024) fsz = 1024 * 1024; // 1 MB cap
    char *buf = malloc(fsz + 1);
    if (!buf) { fclose(f); tv.read_only = true; current_mode = MODE_VIEW_TEXT; return; }
    size_t got = fread(buf, 1, fsz, f);
    fclose(f);
    buf[got] = '\0';

    tv_split_lines(buf, got);
    free(buf);

    // Check write permission
    tv.read_only = (access(path, W_OK) != 0);

    vtree_log("[viewer] opened: %s  size=%ld bytes  lines=%d%s\n",
              path, fsz, tv.line_count, tv.read_only ? "  [read-only]" : "");

    current_mode = MODE_VIEW_TEXT;
}

void viewer_close(void) {
    tv_osk_pending = false;
    tv_menu_open   = false;
    tv_confirm_del = false;
}

// Called from main.c after the OSK completes a line edit (commit)
// main.c must call this when current_mode returns from MODE_OSK with
// tv_osk_pending == true.
void viewer_osk_commit(const char *new_text) {
    if (tv_osk_line >= 0 && tv_osk_line < tv.line_count) {
        vtree_log("[viewer] line %d edited\n", tv_osk_line + 1);
        strncpy(tv.lines[tv_osk_line], new_text, TV_MAX_LINE_LEN - 1);
        tv.lines[tv_osk_line][TV_MAX_LINE_LEN - 1] = '\0';
        tv.modified = true;
    }
    tv_osk_pending = false;
    tv_osk_line    = -1;
    current_mode   = MODE_VIEW_TEXT;
}

bool viewer_osk_is_pending(void) { return tv_osk_pending; }
int  viewer_osk_line_index(void) { return tv_osk_line; }

// Read-only accessor for a line's text — used by main.c to seed the OSK
const char *viewer_get_line(int idx) {
    if (idx < 0 || idx >= tv.line_count) return "";
    return tv.lines[idx];
}

// ---------------------------------------------------------------------------
// Line operations
// ---------------------------------------------------------------------------

static void tv_op_new_line(void) {
    if (tv.line_count >= TV_MAX_LINES) return;
    int ins = tv.cur_line + 1;
    for (int i = tv.line_count; i > ins; i--)
        memcpy(tv.lines[i], tv.lines[i - 1], TV_MAX_LINE_LEN);
    tv.lines[ins][0] = '\0';
    tv.line_count++;
    tv.cur_line = ins;
    tv.modified = true;
    tv_clamp_scroll();
}

static void tv_op_duplicate_line(void) {
    if (tv.line_count >= TV_MAX_LINES) return;
    int ins = tv.cur_line + 1;
    for (int i = tv.line_count; i > ins; i--)
        memcpy(tv.lines[i], tv.lines[i - 1], TV_MAX_LINE_LEN);
    memcpy(tv.lines[ins], tv.lines[tv.cur_line], TV_MAX_LINE_LEN);
    tv.line_count++;
    tv.cur_line = ins;
    tv.modified = true;
    tv_clamp_scroll();
}

static void tv_op_delete_line(void) {
    for (int i = tv.cur_line; i < tv.line_count - 1; i++)
        memcpy(tv.lines[i], tv.lines[i + 1], TV_MAX_LINE_LEN);
    tv.line_count--;
    if (tv.line_count == 0) { tv.lines[0][0] = '\0'; tv.line_count = 1; }
    if (tv.cur_line >= tv.line_count) tv.cur_line = tv.line_count - 1;
    tv.modified = true;
    tv_clamp_scroll();
}

// ---------------------------------------------------------------------------
// Menu overlay draw (called from viewer_draw when tv_menu_open)
// ---------------------------------------------------------------------------

#define TV_MENU_ITEMS 3
static const char *tv_menu_labels[TV_MENU_ITEMS] = {
    "Viewer_MenuNewLine",
    "Viewer_MenuDuplicate",
    "Viewer_MenuDelete",
};

static void tv_draw_menu(void) {
    // Dim backdrop
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 140);
    SDL_Rect scr = {0, 0, cfg.screen_w, cfg.screen_h};
    SDL_RenderFillRect(renderer, &scr);
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);

    int lh  = cfg.font_size_menu + 12;
    int hh  = cfg.font_size_header + 16;
    int mw  = cfg.screen_w - 80;
    int mh  = hh + TV_MENU_ITEMS * lh + 10;
    int mx  = (cfg.screen_w - mw) / 2;
    int my  = (cfg.screen_h - mh) / 2;

    SDL_Rect mbg  = {mx, my, mw, mh};
    SDL_Rect head = {mx, my, mw, hh};
    SDL_Rect body = {mx, my + hh, mw, mh - hh};

    SDL_SetRenderDrawColor(renderer,
        cfg.theme.header_bg.r, cfg.theme.header_bg.g, cfg.theme.header_bg.b, 255);
    SDL_RenderFillRect(renderer, &head);
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer,
        cfg.theme.menu_bg.r, cfg.theme.menu_bg.g, cfg.theme.menu_bg.b, cfg.theme.menu_bg.a);
    SDL_RenderFillRect(renderer, &body);
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(renderer,
        cfg.theme.menu_border.r, cfg.theme.menu_border.g, cfg.theme.menu_border.b, 255);
    SDL_RenderDrawRect(renderer, &mbg);

    draw_txt_clipped(font_header, tr("Viewer_MenuTitle"),
                     mx + 10, my + (hh - cfg.font_size_header) / 2, mw - 20, cfg.theme.text);

    SDL_RenderSetClipRect(renderer, &mbg);
    for (int i = 0; i < TV_MENU_ITEMS; i++) {
        int iy = my + hh + 5 + i * lh;
        if (i == tv_menu_sel) {
            SDL_SetRenderDrawColor(renderer,
                cfg.theme.highlight_bg.r, cfg.theme.highlight_bg.g,
                cfg.theme.highlight_bg.b, 255);
            SDL_Rect hr = {mx + 1, iy, mw - 2, lh};
            SDL_RenderFillRect(renderer, &hr);
        }
        SDL_Color lc = (i == tv_menu_sel) ? cfg.theme.highlight_text : cfg.theme.text;
        draw_txt_clipped(font_menu, tr(tv_menu_labels[i]),
                         mx + 16, iy + (lh - cfg.font_size_menu) / 2, mw - 32, lc);
    }
    SDL_RenderSetClipRect(renderer, NULL);

    // Delete-confirmation prompt drawn below the menu
    if (tv_confirm_del) {
        int ph = cfg.font_size_footer + 14;
        int py = my + mh + 6;
        if (py + ph > cfg.screen_h) py = my - ph - 6;
        SDL_Rect pbg = {mx, py, mw, ph};
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(renderer,
            cfg.theme.highlight_bg.r, cfg.theme.highlight_bg.g,
            cfg.theme.highlight_bg.b, 210);
        SDL_RenderFillRect(renderer, &pbg);
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
        char del_hint[128];
        snprintf(del_hint, sizeof(del_hint), tr("Viewer_MenuDeleteConfirm"),
                 btn_label(cfg.k_confirm), btn_label(cfg.k_back));
        draw_txt_clipped(font_footer, del_hint,
                         mx + 10, py + (ph - cfg.font_size_footer) / 2,
                         mw - 20, cfg.theme.highlight_text);
    }
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------
void viewer_handle_button(SDL_GameControllerButton btn,
                          bool *dpad_up_held, bool *dpad_down_held,
                          bool *dpad_left_held, bool *dpad_right_held,
                          Uint32 now) {
    (void)dpad_up_held; (void)dpad_down_held;
    (void)dpad_left_held; (void)dpad_right_held;
    (void)now;

    // ── Menu overlay input ────────────────────────────────────────────────────
    if (tv_menu_open) {
        if (tv_confirm_del) {
            // Waiting for delete confirmation
            if (btn == cfg.k_confirm) {
                tv_op_delete_line();
                tv_confirm_del = false;
                tv_menu_open   = false;
            } else if (btn == cfg.k_back || btn == cfg.k_menu) {
                tv_confirm_del = false;
                tv_menu_open   = false;
            }
            return;
        }
        if (btn == cfg.k_back || btn == cfg.k_menu) {
            tv_menu_open = false;
        } else if (btn == SDL_CONTROLLER_BUTTON_DPAD_UP) {
            tv_menu_sel = (tv_menu_sel - 1 + TV_MENU_ITEMS) % TV_MENU_ITEMS;
        } else if (btn == SDL_CONTROLLER_BUTTON_DPAD_DOWN) {
            tv_menu_sel = (tv_menu_sel + 1) % TV_MENU_ITEMS;
        } else if (btn == cfg.k_confirm) {
            if (tv_menu_sel == 0) {         // New Line
                tv_op_new_line();
                tv_menu_open = false;
            } else if (tv_menu_sel == 1) {  // Duplicate Line
                tv_op_duplicate_line();
                tv_menu_open = false;
            } else if (tv_menu_sel == 2) {  // Delete Line
                if (tv.lines[tv.cur_line][0] == '\0') {
                    tv_op_delete_line();    // empty line — delete immediately
                    tv_menu_open = false;
                } else {
                    tv_confirm_del = true;  // non-empty — ask first
                }
            }
        }
        return;
    }

    if (btn == cfg.k_back) {
        current_mode = MODE_EXPLORER;
        return;
    }

    if (btn == cfg.k_menu && !tv.read_only) {
        tv_menu_open   = true;
        tv_menu_sel    = 0;
        tv_confirm_del = false;
        return;
    }
    // k_menu on read-only still closes (original behaviour)
    if (btn == cfg.k_menu) {
        current_mode = MODE_EXPLORER;
        return;
    }

    if (btn == SDL_CONTROLLER_BUTTON_START) {
        if (tv.modified && !tv.read_only) {
            tv_save();
            tv.modified = false;
        }
        return;
    }

    if (btn == cfg.k_confirm && !tv.read_only) {
        tv_osk_pending = true;
        tv_osk_line    = tv.cur_line;
        return;
    }

    if (btn == SDL_CONTROLLER_BUTTON_DPAD_UP) {
        tv.cur_line--;
        if (tv.cur_line < 0) tv.cur_line = 0;
        tv_clamp_scroll();
    }
    else if (btn == SDL_CONTROLLER_BUTTON_DPAD_DOWN) {
        tv.cur_line++;
        if (tv.cur_line >= tv.line_count) tv.cur_line = tv.line_count - 1;
        tv_clamp_scroll();
    }
    else if (btn == cfg.k_pgup) {
        int vis = tv_visible_lines();
        tv.top_line = SDL_max(0, tv.top_line - vis);
        tv.cur_line = SDL_min(tv.line_count - 1, tv.top_line + vis - 1);
        tv_clamp_scroll();
    }
    else if (btn == cfg.k_pgdn) {
        int vis = tv_visible_lines();
        tv.top_line = SDL_min(SDL_max(0, tv.line_count - vis), tv.top_line + vis);
        tv.cur_line = tv.top_line;
        tv_clamp_scroll();
    }
    else if (btn == SDL_CONTROLLER_BUTTON_DPAD_LEFT && !tv.modified) {
        int step = (font_list ? TTF_FontHeight(font_list) : cfg.font_size_list) * 4;
        tv.x_off = SDL_max(0, tv.x_off - step);
    }
    else if (btn == SDL_CONTROLLER_BUTTON_DPAD_RIGHT && !tv.modified) {
        int step = (font_list ? TTF_FontHeight(font_list) : cfg.font_size_list) * 4;
        tv.x_off += step;
    }
}

void viewer_handle_repeat(Uint32 now) {
    // D-pad repeat is handled by main.c calling viewer_handle_button with
    // the same direction button — nothing special needed here beyond that hook.
    (void)now;
}

// ---------------------------------------------------------------------------
// Render
// ---------------------------------------------------------------------------
void viewer_draw(void) {
    SDL_SetRenderDrawColor(renderer, cfg.theme.bg.r, cfg.theme.bg.g, cfg.theme.bg.b, 255);
    SDL_RenderClear(renderer);

    int head_h = cfg.font_size_header + 12;
    int foot_h = cfg.font_size_footer + 16;
    int item_h = tv_item_h();
    int vis    = tv_visible_lines();

    // ── Header ───────────────────────────────────────────────────────────────
    SDL_SetRenderDrawColor(renderer,
        cfg.theme.header_bg.r, cfg.theme.header_bg.g, cfg.theme.header_bg.b, 255);
    SDL_Rect hr = {0, 0, cfg.screen_w, head_h};
    SDL_RenderFillRect(renderer, &hr);

    // Filename (basename only to save space)
    const char *bname = strrchr(tv.path, '/');
    bname = bname ? bname + 1 : tv.path;
    char title[MAX_PATH + 16];
    snprintf(title, sizeof(title), "%s%s%s",
             bname,
             tv.modified   ? tr("Viewer_Modified") : "",
             tv.read_only  ? tr("Viewer_ReadOnly")  : "");
    draw_txt(font_header, title, 6, (head_h - cfg.font_size_header) / 2, cfg.theme.text);

    // Line/total right-aligned
    char pos[32]; snprintf(pos, sizeof(pos), "L%d/%d", tv.cur_line + 1, tv.line_count);
    int pw = 0; if (font_header) TTF_SizeText(font_header, pos, &pw, NULL);
    draw_txt(font_header, pos, cfg.screen_w - pw - 8,
             (head_h - cfg.font_size_header) / 2, cfg.theme.marked);

    // ── Line area ────────────────────────────────────────────────────────────
    int line_num_w = cfg.font_size_list * 4;  // width reserved for line numbers (4 chars wide)
    int text_x     = 6 + line_num_w;

    for (int i = 0; i < vis + 1; i++) {
        int li = tv.top_line + i;
        if (li >= tv.line_count) break;
        int ry = head_h + i * item_h;

        // Alternating row tint
        if (i % 2 != 0) {
            SDL_SetRenderDrawColor(renderer,
                cfg.theme.alt_bg.r, cfg.theme.alt_bg.g, cfg.theme.alt_bg.b, 255);
            SDL_Rect ar = {0, ry, cfg.screen_w, item_h};
            SDL_RenderFillRect(renderer, &ar);
        }
        // Cursor row highlight
        if (li == tv.cur_line) {
            SDL_SetRenderDrawColor(renderer,
                cfg.theme.highlight_bg.r, cfg.theme.highlight_bg.g,
                cfg.theme.highlight_bg.b, 255);
            SDL_Rect cr = {0, ry, cfg.screen_w, item_h};
            SDL_RenderFillRect(renderer, &cr);
        }

        // Line number
        char lnum[12]; snprintf(lnum, sizeof(lnum), "%4d", li + 1);
        draw_txt(font_list, lnum, 4, ry + 2,
                 li == tv.cur_line ? cfg.theme.highlight_text : cfg.theme.text_disabled);

        // Line text — offset by x_off for horizontal panning
        SDL_Color tc = li == tv.cur_line ? cfg.theme.highlight_text : cfg.theme.text;
        SDL_Rect clip = {text_x, ry, cfg.screen_w - text_x - 4, item_h};
        SDL_RenderSetClipRect(renderer, &clip);
        draw_txt(font_list, tv.lines[li], text_x - tv.x_off, ry + 2, tc);
        SDL_RenderSetClipRect(renderer, NULL);
    }

    // ── Footer ────────────────────────────────────────────────────────────────
    SDL_SetRenderDrawColor(renderer,
        cfg.theme.header_bg.r, cfg.theme.header_bg.g, cfg.theme.header_bg.b, 255);
    SDL_Rect fr = {0, cfg.screen_h - foot_h, cfg.screen_w, foot_h};
    SDL_RenderFillRect(renderer, &fr);

    char hint[256];
    const char *lbl_pgup = btn_label(cfg.k_pgup);
    const char *lbl_pgdn = btn_label(cfg.k_pgdn);
    const char *lbl_conf = btn_label(cfg.k_confirm);
    const char *lbl_back = btn_label(cfg.k_back);
    if (tv.read_only)
        snprintf(hint, sizeof(hint), tr("Viewer_HintReadOnly"), lbl_pgup, lbl_pgdn, lbl_back);
    else if (tv.modified)
        snprintf(hint, sizeof(hint), tr("Viewer_HintModified"), lbl_pgup, lbl_pgdn, lbl_conf, lbl_back);
    else
        snprintf(hint, sizeof(hint), tr("Viewer_HintEdit"), lbl_pgup, lbl_pgdn, lbl_conf, lbl_back);
    draw_txt_clipped(font_footer, hint, 8, cfg.screen_h - foot_h + (foot_h - cfg.font_size_footer) / 2,
             cfg.screen_w - 16, cfg.theme.text_disabled);

    if (tv_menu_open)
        tv_draw_menu();
}