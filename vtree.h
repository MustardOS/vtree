#ifndef VTREE_H
#define VTREE_H

#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <SDL2/SDL_image.h>
#include <stdbool.h>

#define MAX_PATH           1024
#define MAX_FILES          1000   // imgview sibling cap — do not remove
#define FILES_INIT_CAP     512
#define MAX_CLIPBOARD      100
#define GLYPH_CACHE_SIZE   128
#define THEME_PRESET_COUNT 1   // only the built-in Dark fallback lives in C now

typedef enum {
    MODE_EXPLORER,
    MODE_CONTEXT_MENU,
    MODE_SETTINGS,
    MODE_OSK,
    MODE_VIEW_CHOOSE,   // "Open as: Text / Hex / Cancel" picker
    MODE_VIEW_TEXT,     // text viewer / editor
    MODE_VIEW_HEX,      // hex viewer / editor
    MODE_VIEW_IMAGE,    // image viewer
    MODE_SNAKE,         // easter egg
} AppMode;
typedef enum { OP_NONE, OP_COPY, OP_CUT } FileOp;
typedef enum { OSK_LOWER, OSK_UPPER, OSK_SPECIAL, OSK_LAYER_COUNT } OSKLayer;
typedef enum { OSK_FOR_RENAME, OSK_FOR_TEXT_EDIT, OSK_FOR_NEW_FILE, OSK_FOR_NEW_DIR, OSK_FOR_SETTINGS_PATH } OSKPurpose;

typedef struct {
    char     buf[256];      // text buffer
    int      len;           // strlen(buf)
    int      cursor;        // insertion point (0..len), caret position
    bool     insert_mode;   // true = insert (default), false = overwrite
    bool     kb_visible;    // keyboard grid visible (can be toggled)
    int      scroll_x;      // horizontal pixel scroll offset for input field
    int      row, col;      // grid focus: row 0..OSK_CHAR_ROWS-1 or OSK_CHAR_ROWS (action)
    char     orig_name[256];
    char     dir_path[MAX_PATH];
    OSKLayer layer;
} OSKState;

extern OSKState   osk;
extern OSKPurpose osk_purpose;

typedef struct { char name[256]; bool is_dir; bool is_link; bool marked; long long size; } FileEntry;
typedef struct { char current_path[MAX_PATH]; FileEntry *files; int file_capacity, file_count, selected_index, scroll_offset; } AppState;

typedef struct {
    SDL_Color bg, alt_bg, header_bg, text, text_disabled, link, highlight_bg, highlight_text, marked;
    SDL_Color menu_bg;
    SDL_Color menu_border;
    // Hex-viewer byte-category colours (optional per theme; fallback = built-in Dark values)
    SDL_Color hex_zero;    // 0x00          — null bytes
    SDL_Color hex_ctrl;    // 0x01–0x1F     — control codes
    SDL_Color hex_space;   // 0x20          — space character
    SDL_Color hex_punct;   // punctuation / symbols
    SDL_Color hex_digit;   // 0x30–0x39     — ASCII digits
    SDL_Color hex_letter;  // A–Z, a–z      — ASCII letters
    SDL_Color hex_high;    // 0x80–0xFE     — high/non-ASCII bytes
    SDL_Color hex_full;    // 0xFF          — all-ones byte
} Theme;

// ---------------------------------------------------------------------------
// Named theme loaded from file (config.ini [Theme.Name] or theme.ini)
// ---------------------------------------------------------------------------
#define MAX_NAMED_THEMES 32
#define MAX_THEME_NAME   64

typedef struct {
    char  name[MAX_THEME_NAME];
    Theme colors;
} NamedTheme;

extern NamedTheme named_themes[MAX_NAMED_THEMES];
extern int        named_theme_count;
extern int        current_named_theme;  // index into named_themes[], -1 = custom

typedef struct {
    int screen_w, screen_h;
    int font_size_list, font_size_header, font_size_footer, font_size_menu;
    int font_size_hex;    // independent font size for the hex viewer
    char start_left[MAX_PATH], start_right[MAX_PATH];
    char gamecontrollerdb[MAX_PATH];   // path to gamecontrollerdb.txt; "" = use CWD default
    Theme theme;
    // Explorer / global keys
    SDL_GameControllerButton k_confirm, k_back, k_menu, k_mark;
    SDL_GameControllerButton k_pgup, k_pgdn;   // page up / page down in file manager + viewers
    SDL_GameControllerButton k_menu2;          // system menu key (Settings/About/Exit) in two-menu mode
    // OSK-specific keys (independent, fully rebindable)
    SDL_GameControllerButton osk_k_type;    // press selected key       (default: a)
    SDL_GameControllerButton osk_k_bksp;   // backspace at cursor      (default: x)
    SDL_GameControllerButton osk_k_shift;  // cycle layer              (default: y)
    SDL_GameControllerButton osk_k_cancel; // cancel OSK               (default: b)
    SDL_GameControllerButton osk_k_toggle; // show / hide keyboard     (default: back)
    SDL_GameControllerButton osk_k_ins;    // toggle insert/overwrite  (default: leftshoulder)
    // Extra file-type extensions (appended to built-in lists at runtime)
#define MAX_EXTRA_EXTS 64
#define MAX_EXT_LEN    16
    char font_path[MAX_PATH];              // basename of active font file (e.g. "font.ttf")
    char extra_image_exts[MAX_EXTRA_EXTS][MAX_EXT_LEN];
    int  extra_image_ext_count;
    char extra_text_exts[MAX_EXTRA_EXTS][MAX_EXT_LEN];
    int  extra_text_ext_count;
    bool show_hidden;    // show dotfiles / hidden entries
    bool remember_dirs;  // save pane paths on exit and restore on next launch
    bool exec_scripts;   // allow executing .sh files (experimental)
    bool single_pane;    // show only one full-width pane instead of the split view
    bool two_menu_mode;  // k_menu goes to file-ops directly; k_menu2 opens system menu
    bool tint_icons;     // tint file-list icons to match their row's theme colour
    int  rotation;       // display rotation: 0=none, 1=90°CW, 2=180°, 3=270°CW
    char language_name[64];  // display name of the active language (e.g. "English")
} AppConfig;

typedef struct {
    char         text[MAX_PATH];
    TTF_Font    *font;
    SDL_Color    color;
    SDL_Texture *texture;
    int          w, h;
    Uint32       last_used;
} GlyphEntry;

extern GlyphEntry glyph_cache[GLYPH_CACHE_SIZE];
extern Uint32     glyph_frame;

extern AppConfig cfg;
extern AppState  panes[2];
extern int       active_pane;
extern AppMode   current_mode;
extern bool      debug_mode;
extern FILE     *debug_log_file;
void vtree_log(const char *fmt, ...);
extern bool      delete_confirm_active;
extern int       settings_index;

// Legacy single-preset name table (just "Dark" now; kept so settings UI compiles)
extern const char *theme_preset_names[THEME_PRESET_COUNT];
extern int         current_theme_preset;   // -1 = custom/none

typedef struct {
    char src_paths[MAX_CLIPBOARD][MAX_PATH];
    char names[MAX_CLIPBOARD][256];
    int  count;
    FileOp op;
} Clipboard;
extern Clipboard clip;

// config.c
void load_config();
void save_config();
void apply_theme_preset(int idx);       // applies named_themes[idx]

// fileop.c
void  load_dir(int p_idx, const char *path);
int   copy_path(const char *src, const char *dest);
int   delete_path(const char *path);
char *trim(char *str);
void  format_size(long long bytes, char *out);
void  join_path(char *out, const char *dir, const char *name);

// lang.c — i18n
#include "lang.h"

// main.c
void destroy_glyph_cache();
void draw_txt(TTF_Font *f, const char *txt, int x, int y, SDL_Color col);
void draw_txt_clipped(TTF_Font *f, const char *txt, int x, int y, int max_w, SDL_Color col);
extern SDL_Renderer      *renderer;
extern SDL_Window        *window;
extern SDL_GameController *pad;
extern TTF_Font *font_list, *font_header, *font_footer, *font_menu, *font_hex;
extern char      vtree_font_path[MAX_PATH];  // full resolved path of the active font file
extern char      vtree_exe_dir[MAX_PATH];    // directory containing the vtree binary
extern Uint32 glyph_frame;
// File-type icon textures (NULL = not loaded, fall back to tex_file)
extern SDL_Texture *tex_file, *tex_folder, *tex_img, *tex_txt, *tex_dirup;

// viewer.c  — text viewer / editor
void        viewer_open(const char *path);
void        viewer_close(void);
void        viewer_osk_commit(const char *new_text);
bool        viewer_osk_is_pending(void);
int         viewer_osk_line_index(void);
const char *viewer_get_line(int idx);
void        viewer_handle_button(SDL_GameControllerButton btn, bool *dpad_up_held, bool *dpad_down_held,
                                 bool *dpad_left_held, bool *dpad_right_held, Uint32 now);
void        viewer_handle_repeat(Uint32 now);
void        viewer_draw(void);

// hexview.c — hex viewer / editor
void hexview_open(const char *path);
void hexview_close(void);
void hexview_handle_button(SDL_GameControllerButton btn, bool *dpad_up_held, bool *dpad_down_held,
                           bool *dpad_left_held, bool *dpad_right_held, Uint32 now);
void hexview_handle_repeat(Uint32 now);
void hexview_draw(void);

// keyboard.c — on-screen keyboard
const char *btn_label(SDL_GameControllerButton btn);
void osk_init_common(void);
void osk_enter(const char *dir, const char *filename);
void osk_enter_tv(const char *line_text);
void osk_enter_new(const char *dir, bool is_dir);
void osk_enter_path(char *target, const char *current_val);
extern char *osk_path_target;
void osk_type(char ch);
void osk_backspace(void);
void osk_cycle_layer(void);
void osk_press(void);
void osk_move(int dr, int dc);
void osk_cursor_left(void);
void osk_cursor_right(void);
void osk_confirm(void);
void draw_osk(void);

// snake.c — easter egg
void snake_enter(void);
void snake_handle_button(SDL_GameControllerButton btn);
void snake_tick(Uint32 now);
void snake_draw(void);

// imgview.c — image viewer
void imgview_open(const char *path);
void imgview_close(void);
void imgview_handle_button(SDL_GameControllerButton btn, Uint32 now);
void imgview_draw(void);

#endif
