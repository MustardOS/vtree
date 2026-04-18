// keyboard.c — On-screen keyboard (OSK): key tables, logic, and render
// ---------------------------------------------------------------------------
// The event-loop wiring (input handling, auto-repeat state) lives in main.c
// following the same pattern as viewer.c / hexview.c.
// ---------------------------------------------------------------------------

#include "vtree.h"
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>

// ---------------------------------------------------------------------------
// OSK: keymaps (3 layers × 4 rows × 10 cols)
// All rows are exactly 10 wide — no variable-length complexity.
// Lower/Upper: numbers at top, QWERTY below, filename punctuation on bottom rows.
// Special: shifted-number symbols at top, brackets/math row 1, punctuation row 2,
//          numbers as fallback row 3 (avoids a Y-press to type e.g. "v2").
// ---------------------------------------------------------------------------
#define OSK_COLS      10
#define OSK_CHAR_ROWS  4

static const char osk_keys[OSK_LAYER_COUNT][OSK_CHAR_ROWS][OSK_COLS] = {
    { // OSK_LOWER  (Y → OSK_UPPER)
        {'1','2','3','4','5','6','7','8','9','0'},
        {'q','w','e','r','t','y','u','i','o','p'},
        {'a','s','d','f','g','h','j','k','l','.'},
        {'z','x','c','v','b','n','m',',','-','_'},
    },
    { // OSK_UPPER  (Y → OSK_SPECIAL)
        {'1','2','3','4','5','6','7','8','9','0'},
        {'Q','W','E','R','T','Y','U','I','O','P'},
        {'A','S','D','F','G','H','J','K','L','.'},
        {'Z','X','C','V','B','N','M',',','-','_'},
    },
    { // OSK_SPECIAL  (Y → OSK_LOWER)
        {'!','@','#','$','%','^','&','*','(',')'},
        {'`','~','+','=','{','}','[',']','<','>'},
        {'|','\\',':',';','"','\'','?','/','-','_'},
        {'1','2','3','4','5','6','7','8','9','0'},
    },
};

static const char *osk_layer_labels[OSK_LAYER_COUNT] = { "OSK_Layer_Lower", "OSK_Layer_Upper", "OSK_Layer_Special" };

// Action row (below the key grid): TAB | SPACE (wide) | ENTER
#define OSK_ACT_COUNT  3
#define OSK_ACT_TAB    0
#define OSK_ACT_SPACE  1
#define OSK_ACT_ENTER  2
static const char *osk_act_labels[OSK_ACT_COUNT] = { "OSK_Tab", "OSK_Space", "OSK_Enter" };

// ---------------------------------------------------------------------------
// Globals — extern-declared in vtree.h
// ---------------------------------------------------------------------------
OSKState  osk;
OSKPurpose osk_purpose  = OSK_FOR_RENAME;
char      *osk_path_target = NULL;

// ---------------------------------------------------------------------------
// OSK — logic
// ---------------------------------------------------------------------------

// Common init shared by all entry points
void osk_init_common(void) {
    osk.row         = 0;
    osk.col         = 0;
    osk.layer       = OSK_LOWER;
    osk.insert_mode = true;   // start in INSERT mode
    osk.kb_visible  = true;   // keyboard shown by default
}

// Insert character at cursor position (insert or overwrite)
void osk_type(char ch) {
    if (osk.insert_mode) {
        if (osk.len >= 254) return;
        memmove(&osk.buf[osk.cursor + 1], &osk.buf[osk.cursor], osk.len - osk.cursor + 1);
        osk.buf[osk.cursor] = ch;
        osk.len++;
        osk.cursor++;
    } else {
        if (osk.cursor < osk.len) {
            osk.buf[osk.cursor] = ch;
            osk.cursor++;
        } else if (osk.len < 254) {
            osk.buf[osk.len] = ch;
            osk.len++;
            osk.buf[osk.len] = '\0';
            osk.cursor++;
        }
    }
}

// Backspace: delete character to the left of cursor
void osk_backspace(void) {
    if (osk.cursor <= 0) return;
    memmove(&osk.buf[osk.cursor - 1], &osk.buf[osk.cursor], osk.len - osk.cursor + 1);
    osk.len--;
    osk.cursor--;
}

void osk_cycle_layer(void) {
    osk.layer = (OSKLayer)((osk.layer + 1) % OSK_LAYER_COUNT);
}

// Move text cursor (caret) left/right within buffer
void osk_cursor_left(void)  { if (osk.cursor > 0)       osk.cursor--; }
void osk_cursor_right(void) { if (osk.cursor < osk.len)  osk.cursor++; }

// D-pad navigation within the keyboard grid
void osk_move(int dr, int dc) {
    if (dr != 0) {
        int nr = (osk.row + dr + OSK_CHAR_ROWS + 1) % (OSK_CHAR_ROWS + 1);
        if (osk.row < OSK_CHAR_ROWS && nr == OSK_CHAR_ROWS)
            osk.col = (osk.col * OSK_ACT_COUNT) / OSK_COLS;
        else if (osk.row == OSK_CHAR_ROWS && nr < OSK_CHAR_ROWS)
            osk.col = (osk.col * OSK_COLS) / OSK_ACT_COUNT;
        osk.row = nr;
    }
    if (dc != 0) {
        int max = (osk.row == OSK_CHAR_ROWS) ? OSK_ACT_COUNT : OSK_COLS;
        osk.col = (osk.col + dc + max) % max;
    }
}

// ---------------------------------------------------------------------------
// OSK confirm — commit based on purpose (defined before osk_press)
// ---------------------------------------------------------------------------
void osk_confirm(void) {
    if (osk_purpose == OSK_FOR_RENAME) {
        if (osk.len == 0) return;
        char old_path[MAX_PATH], new_path[MAX_PATH];
        join_path(old_path, osk.dir_path, osk.orig_name);
        join_path(new_path, osk.dir_path, osk.buf);
        if (strcmp(old_path, new_path) != 0) {
            vtree_log("[rename] %s -> %s\n", old_path, new_path);
            if (rename(old_path, new_path) != 0)
                vtree_log("[rename] FAILED (errno %d: %s)\n", errno, strerror(errno));
            load_dir(0, panes[0].current_path);
            load_dir(1, panes[1].current_path);
        }
        current_mode = MODE_EXPLORER;
    } else if (osk_purpose == OSK_FOR_NEW_FILE) {
        if (osk.len == 0) { current_mode = MODE_EXPLORER; osk_purpose = OSK_FOR_RENAME; return; }
        char new_path[MAX_PATH];
        join_path(new_path, osk.dir_path, osk.buf);
        vtree_log("[newfile] %s\n", new_path);
        FILE *f = fopen(new_path, "wb");
        if (f) fclose(f);
        else vtree_log("[newfile] FAILED (errno %d: %s)\n", errno, strerror(errno));
        load_dir(0, panes[0].current_path);
        load_dir(1, panes[1].current_path);
        current_mode = MODE_EXPLORER;
    } else if (osk_purpose == OSK_FOR_NEW_DIR) {
        if (osk.len == 0) { current_mode = MODE_EXPLORER; osk_purpose = OSK_FOR_RENAME; return; }
        char new_path[MAX_PATH];
        join_path(new_path, osk.dir_path, osk.buf);
        vtree_log("[mkdir] %s\n", new_path);
        if (mkdir(new_path, 0755) != 0)
            vtree_log("[mkdir] FAILED (errno %d: %s)\n", errno, strerror(errno));
        load_dir(0, panes[0].current_path);
        load_dir(1, panes[1].current_path);
        current_mode = MODE_EXPLORER;
    } else if (osk_purpose == OSK_FOR_SETTINGS_PATH) {
        if (osk_path_target && osk.len > 0)
            strncpy(osk_path_target, osk.buf, MAX_PATH - 1);
        osk_path_target = NULL;
        current_mode    = MODE_SETTINGS;
    } else { // OSK_FOR_TEXT_EDIT
        viewer_osk_commit(osk.buf);
    }
    osk_purpose = OSK_FOR_RENAME;
}

// A-button / action-row press
void osk_press(void) {
    if (osk.row < OSK_CHAR_ROWS) {
        osk_type(osk_keys[osk.layer][osk.row][osk.col]);
    } else {
        switch (osk.col) {
            case OSK_ACT_TAB:   osk_type('\t'); break;
            case OSK_ACT_SPACE: osk_type(' ');  break;
            case OSK_ACT_ENTER: osk_confirm();  break;
        }
    }
}

// ---------------------------------------------------------------------------
// OSK entry points
// ---------------------------------------------------------------------------

void osk_enter(const char *dir, const char *filename) {
    memset(&osk, 0, sizeof(osk));
    strncpy(osk.dir_path,  dir,      MAX_PATH - 1); osk.dir_path[MAX_PATH - 1] = '\0';
    strncpy(osk.orig_name, filename, 255);           osk.orig_name[255] = '\0';
    strncpy(osk.buf,       filename, 255);           osk.buf[255] = '\0';
    osk.len    = (int)strlen(osk.buf);
    osk.cursor = osk.len;
    osk_init_common();
    current_mode = MODE_OSK;
}

// Entry point for settings path editing
void osk_enter_path(char *target, const char *current_val) {
    memset(&osk, 0, sizeof(osk));
    strncpy(osk.buf,       current_val, 255); osk.buf[255] = '\0';
    strncpy(osk.orig_name, current_val, 255); osk.orig_name[255] = '\0';
    osk.len         = (int)strlen(osk.buf);
    osk.cursor      = osk.len;
    osk_path_target = target;
    osk_init_common();
    osk_purpose  = OSK_FOR_SETTINGS_PATH;
    current_mode = MODE_OSK;
}

// Entry point for text-editor line editing
void osk_enter_tv(const char *line_text) {
    memset(&osk, 0, sizeof(osk));
    strncpy(osk.buf,       line_text, 255); osk.buf[255] = '\0';
    strncpy(osk.orig_name, line_text, 255); osk.orig_name[255] = '\0';
    osk.len    = (int)strlen(osk.buf);
    osk.cursor = osk.len;
    osk_init_common();
    osk_purpose  = OSK_FOR_TEXT_EDIT;
    current_mode = MODE_OSK;
}

// Entry point for creating a new file or directory
void osk_enter_new(const char *dir, bool is_dir) {
    memset(&osk, 0, sizeof(osk));
    strncpy(osk.dir_path, dir, MAX_PATH - 1); osk.dir_path[MAX_PATH - 1] = '\0';
    osk.buf[0]    = '\0';
    osk.len       = 0;
    osk.cursor    = 0;
    osk_init_common();
    osk_purpose  = is_dir ? OSK_FOR_NEW_DIR : OSK_FOR_NEW_FILE;
    current_mode = MODE_OSK;
}

// ---------------------------------------------------------------------------
// Returns a short, human-friendly label for a gamepad button.
// "back" is shown as "Select" to match common handheld labelling.
// ---------------------------------------------------------------------------
const char *btn_label(SDL_GameControllerButton btn) {
    const char *s = SDL_GameControllerGetStringForButton(btn);
    if (!s) return "?";
    if (strcmp(s, "a")             == 0) return "A";
    if (strcmp(s, "b")             == 0) return "B";
    if (strcmp(s, "x")             == 0) return "X";
    if (strcmp(s, "y")             == 0) return "Y";
    if (strcmp(s, "start")         == 0) return "Start";
    if (strcmp(s, "guide")         == 0) return "Guide";
    if (strcmp(s, "back")          == 0) return "Select";
    if (strcmp(s, "leftshoulder")  == 0) return "L1";
    if (strcmp(s, "rightshoulder") == 0) return "R1";
    if (strcmp(s, "leftstick")     == 0) return "L3";
    if (strcmp(s, "rightstick")    == 0) return "R3";
    if (strcmp(s, "misc1")         == 0) return "Extra";
    return s;
}

// ---------------------------------------------------------------------------
// OSK — render
// ---------------------------------------------------------------------------
void draw_osk(void) {
    const int PAD    = 8;
    const int BORDER = 2;

    // ── Geometry ─────────────────────────────────────────────────────────────
    // Derive the dialog width FROM the key grid, not the other way round.
    // This keeps the box tight — no wasted space left/right of the keys.
    // Keys are sized against a comfortable fraction of screen width first.
    int cw_key = ((cfg.screen_w - 100 - PAD * 2) / OSK_COLS) * 82 / 100;
    int ch         = cw_key;               // HEIGHT == WIDTH → perfect squares
    int grid_px_w  = cw_key * OSK_COLS;    // total key-grid pixel width

    // Dialog content width equals the grid; box = grid + side padding.
    int cw   = grid_px_w;
    int dw   = cw + PAD * 2;
    int dx   = (cfg.screen_w - dw) / 2;

    int th  = cfg.font_size_header + 2; // title text height (compact)
    int fh  = cfg.font_size_list   + 8; // input field height
    int ach = ch;                        // action row — same height as key rows

    // Total dialog height — keyboard section is conditional
    int kb_h = 0;
    if (osk.kb_visible)
        kb_h = OSK_CHAR_ROWS * ch + ach;

    int dh = PAD + th + PAD + fh + PAD + kb_h + PAD;

    // ── Bottom-aligned: sit just above the explorer footer bar ───────────────
    int explorer_foot_h = cfg.font_size_footer + 16;
    int dy = cfg.screen_h - explorer_foot_h - dh - 2;

    // ── Darkening backdrop ───────────────────────────────────────────────────
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 160);
    SDL_Rect scr = {0, 0, cfg.screen_w, cfg.screen_h};
    SDL_RenderFillRect(renderer, &scr);

    // ── Dialog body ──────────────────────────────────────────────────────────
    SDL_SetRenderDrawColor(renderer, cfg.theme.menu_bg.r, cfg.theme.menu_bg.g,
                           cfg.theme.menu_bg.b, cfg.theme.menu_bg.a);
    SDL_Rect dlg = {dx, dy, dw, dh};
    SDL_RenderFillRect(renderer, &dlg);
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);

    SDL_SetRenderDrawColor(renderer, cfg.theme.menu_border.r, cfg.theme.menu_border.g,
                           cfg.theme.menu_border.b, 255);
    for (int i = 0; i < BORDER; i++) {
        SDL_Rect b = {dx+i, dy+i, dw-2*i, dh-2*i};
        SDL_RenderDrawRect(renderer, &b);
    }

    int cx = dx + PAD;
    int y  = dy + PAD;

    // ── Title bar ─────────────────────────────────────────────────────────────
    {
        int bar_h = PAD + th + PAD / 2;
        SDL_SetRenderDrawColor(renderer, cfg.theme.header_bg.r, cfg.theme.header_bg.g,
                               cfg.theme.header_bg.b, 255);
        SDL_Rect tb = {dx + BORDER, dy + BORDER, dw - 2*BORDER, bar_h};
        SDL_RenderFillRect(renderer, &tb);
    }

    // Title label — changes with OSK purpose
    const char *title_lbl =
        (osk_purpose == OSK_FOR_RENAME)       ? tr("OSK_Title_Rename")    :
        (osk_purpose == OSK_FOR_NEW_FILE)     ? tr("OSK_Title_NewFile")   :
        (osk_purpose == OSK_FOR_NEW_DIR)      ? tr("OSK_Title_NewFolder") :
        (osk_purpose == OSK_FOR_SETTINGS_PATH)? tr("OSK_Title_Settings")  :
                                                tr("OSK_Title_EditLine");
    draw_txt(font_header, title_lbl, cx, y, cfg.theme.text);

    // Right side of title: layer indicator + INS/OVR badge
    {
        const char *layer_lbl = tr(osk_layer_labels[osk.layer]);
        const char *mode_tag  = osk.insert_mode ? tr("OSK_Mode_Insert") : tr("OSK_Mode_Overwrite");
        SDL_Color   mode_col  = osk.insert_mode
            ? (SDL_Color){80, 200, 80, 255}
            : (SDL_Color){220, 100, 60, 255};
        int lw = 0, mw = 0;
        if (font_header) {
            TTF_SizeText(font_header, layer_lbl, &lw, NULL);
            TTF_SizeText(font_header, mode_tag,  &mw, NULL);
        }
        int right = dx + dw - PAD;
        draw_txt(font_header, mode_tag,   right - mw,          y, mode_col);
        draw_txt(font_header, layer_lbl,  right - mw - 8 - lw, y, cfg.theme.text_disabled);
    }
    y += th + PAD;

    // ── Input field ───────────────────────────────────────────────────────────
    SDL_SetRenderDrawColor(renderer, cfg.theme.alt_bg.r, cfg.theme.alt_bg.g,
                           cfg.theme.alt_bg.b, 255);
    SDL_Rect field = {cx, y, cw, fh};
    SDL_RenderFillRect(renderer, &field);
    SDL_SetRenderDrawColor(renderer, cfg.theme.menu_border.r, cfg.theme.menu_border.g,
                           cfg.theme.menu_border.b, 255);
    SDL_RenderDrawRect(renderer, &field);

    int text_left = cx + 6;
    int text_top  = y + (fh - cfg.font_size_list) / 2;

    if (font_list) {
        SDL_Rect clip_r = {cx + 1, y + 1, cw - 2, fh - 2};
        SDL_RenderSetClipRect(renderer, &clip_r);
        if (osk.len > 0)
            draw_txt(font_list, osk.buf, text_left, text_top, cfg.theme.text);

        // Compute caret pixel x — measure prefix
        int caret_px = text_left;
        if (osk.cursor > 0) {
            char prefix[256];
            int plen = SDL_min(osk.cursor, 255);
            memcpy(prefix, osk.buf, plen);
            prefix[plen] = '\0';
            int pw = 0;
            TTF_SizeText(font_list, prefix, &pw, NULL);
            caret_px = text_left + pw;
        }

        // Draw caret — blinking thin line (INSERT) or block underline (OVERWRITE)
        if ((glyph_frame / 30) % 2 == 0) {
            SDL_SetRenderDrawColor(renderer, cfg.theme.text.r, cfg.theme.text.g, cfg.theme.text.b, 255);
            if (osk.insert_mode) {
                SDL_Rect cur = {caret_px, y + 3, 2, fh - 6};
                SDL_RenderFillRect(renderer, &cur);
            } else {
                int char_w = cfg.font_size_list / 2 + 2;
                if (osk.cursor < osk.len && font_list) {
                    char ch_str[3] = { osk.buf[osk.cursor], '\0', '\0' };
                    TTF_SizeText(font_list, ch_str, &char_w, NULL);
                }
                SDL_Rect cur = {caret_px, y + fh - 5, char_w, 3};
                SDL_RenderFillRect(renderer, &cur);
            }
        }
        SDL_RenderSetClipRect(renderer, NULL);
    }
    y += fh + PAD;

    // ── Keyboard section (only when visible) ──────────────────────────────────
    if (osk.kb_visible) {

        // ── Character grid — SQUARE keys ─────────────────────────────────────
        int grid_h = OSK_CHAR_ROWS * ch;
        int grid_y = y;

        // Grid fill
        SDL_SetRenderDrawColor(renderer, cfg.theme.bg.r, cfg.theme.bg.g, cfg.theme.bg.b, 255);
        SDL_Rect grid_area = {cx, grid_y, grid_px_w, grid_h};
        SDL_RenderFillRect(renderer, &grid_area);

        // Row 0 tint (visual separator from the letter rows below)
        SDL_SetRenderDrawColor(renderer, cfg.theme.alt_bg.r, cfg.theme.alt_bg.g,
                               cfg.theme.alt_bg.b, 255);
        SDL_Rect row0_bg = {cx, grid_y, grid_px_w, ch};
        SDL_RenderFillRect(renderer, &row0_bg);

        // Grid lines — vertical
        SDL_SetRenderDrawColor(renderer, cfg.theme.text_disabled.r, cfg.theme.text_disabled.g,
                               cfg.theme.text_disabled.b, 255);
        for (int c = 1; c < OSK_COLS; c++) {
            int gx = cx + c * cw_key;
            SDL_RenderDrawLine(renderer, gx, grid_y, gx, grid_y + grid_h);
        }
        // Grid lines — horizontal (extra separator below row 0)
        for (int r = 1; r < OSK_CHAR_ROWS; r++) {
            int gy = grid_y + r * ch;
            SDL_RenderDrawLine(renderer, cx, gy, cx + grid_px_w, gy);
            if (r == 1)
                SDL_RenderDrawLine(renderer, cx, gy + 1, cx + grid_px_w, gy + 1);
        }
        SDL_RenderDrawRect(renderer, &grid_area);

        // Selected key highlight
        if (osk.row < OSK_CHAR_ROWS) {
            int kx = cx + osk.col * cw_key;
            int ky = grid_y + osk.row * ch;
            SDL_SetRenderDrawColor(renderer, cfg.theme.highlight_bg.r, cfg.theme.highlight_bg.g,
                                   cfg.theme.highlight_bg.b, 255);
            SDL_Rect hi = {kx + 1, ky + 1, cw_key - 1, ch - 1};
            SDL_RenderFillRect(renderer, &hi);
        }

        // Key labels — centred inside the square cell
        for (int r = 0; r < OSK_CHAR_ROWS; r++) {
            for (int c = 0; c < OSK_COLS; c++) {
                char key[2] = { osk_keys[osk.layer][r][c], '\0' };
                if (!key[0]) continue;
                bool sel = (osk.row == r && osk.col == c);
                int tw = 0, th_k = 0;
                if (font_menu) TTF_SizeText(font_menu, key, &tw, &th_k);
                int kx = cx + c * cw_key + (cw_key - tw) / 2;
                int ky = grid_y + r * ch  + (ch - th_k) / 2;
                draw_txt(font_menu, key, kx, ky, sel ? cfg.theme.highlight_text : cfg.theme.text);
            }
        }
        y = grid_y + grid_h;

        // ── Action row (TAB | SPACE | ENTER) — same height as key rows ────────
        // TAB and ENTER are equal width (2 key cells each); SPACE fills the rest.
        int aw_tab   = cw_key * 2;
        int aw_enter = cw_key * 2;
        int aw_space = grid_px_w - aw_tab - aw_enter;
        int act_x[OSK_ACT_COUNT] = { cx,           cx + aw_tab,           cx + aw_tab + aw_space };
        int act_w[OSK_ACT_COUNT] = { aw_tab,        aw_space,               aw_enter              };

        for (int c = 0; c < OSK_ACT_COUNT; c++) {
            bool sel = (osk.row == OSK_CHAR_ROWS && osk.col == c);
            SDL_SetRenderDrawColor(renderer,
                sel ? cfg.theme.highlight_bg.r : cfg.theme.bg.r,
                sel ? cfg.theme.highlight_bg.g : cfg.theme.bg.g,
                sel ? cfg.theme.highlight_bg.b : cfg.theme.bg.b, 255);
            SDL_Rect ar = {act_x[c] + 1, y + 1, act_w[c] - 2, ach - 2};
            SDL_RenderFillRect(renderer, &ar);
            SDL_SetRenderDrawColor(renderer, cfg.theme.text_disabled.r, cfg.theme.text_disabled.g,
                                   cfg.theme.text_disabled.b, 255);
            SDL_RenderDrawRect(renderer, &ar);
            int lw = 0;
            const char *act_lbl = tr(osk_act_labels[c]);
            if (font_menu) TTF_SizeText(font_menu, act_lbl, &lw, NULL);
            int lx = act_x[c] + (act_w[c] - lw) / 2;
            int ly = y + (ach - cfg.font_size_menu) / 2;
            SDL_Color lc = sel ? cfg.theme.highlight_text
                        : (c == OSK_ACT_SPACE ? cfg.theme.text_disabled : cfg.theme.text);
            draw_txt(font_menu, act_lbl, lx, ly, lc);
        }
    }

    // ── Global footer — overwrite with OSK hint ───────────────────────────────
    int gfoot_h = cfg.font_size_footer + 16;
    int gfoot_y = cfg.screen_h - gfoot_h;
    SDL_SetRenderDrawColor(renderer, cfg.theme.header_bg.r, cfg.theme.header_bg.g,
                           cfg.theme.header_bg.b, 255);
    SDL_Rect gfr = {0, gfoot_y, cfg.screen_w, gfoot_h};
    SDL_RenderFillRect(renderer, &gfr);

    char hint[256];
    const char *ins_state = osk.insert_mode ? tr("OSK_StateIns") : tr("OSK_StateOvr");
    if (osk.kb_visible)
        snprintf(hint, sizeof(hint), tr("OSK_HintVisible"),
                 btn_label(cfg.osk_k_bksp),
                 btn_label(cfg.osk_k_shift),
                 btn_label(cfg.osk_k_toggle),
                 btn_label(cfg.osk_k_ins),
                 ins_state);
    else
        snprintf(hint, sizeof(hint), tr("OSK_HintHidden"),
                 btn_label(cfg.osk_k_bksp),
                 btn_label(cfg.osk_k_toggle),
                 btn_label(cfg.osk_k_ins),
                 ins_state,
                 btn_label(cfg.osk_k_type));
    draw_txt_clipped(font_footer, hint, 12, gfoot_y + (gfoot_h - cfg.font_size_footer) / 2,
             cfg.screen_w - 24, cfg.theme.text_disabled);
}
