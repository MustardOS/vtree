#include "vtree.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <unistd.h>

// ---------------------------------------------------------------------------
// Named-theme table  (loaded from config.ini / theme.ini at runtime)
// ---------------------------------------------------------------------------
NamedTheme named_themes[MAX_NAMED_THEMES];
int        named_theme_count  = 0;
int        current_named_theme = -1;   // -1 until a preset is explicitly applied

// ---------------------------------------------------------------------------
// Built-in fallback (Dark) — used if no themes are found in any config file
// ---------------------------------------------------------------------------
static const Theme dark_fallback = {
    {25,  25,  25,  255}, // bg
    {30,  30,  45,  255}, // alt_bg
    {45,  45,  45,  255}, // header_bg
    {200, 200, 200, 255}, // text
    {100, 100, 100, 255}, // text_disabled
    {95,  200, 200, 255}, // link
    {50,  100, 150, 255}, // highlight_bg
    {255, 255, 255, 255}, // highlight_text
    {255, 215, 0,   255}, // marked
    {30,  30,  40,  240}, // menu_bg
    {100, 100, 100, 255}, // menu_border
    // hex viewer byte-category colours
    {80,  80,  80,  255}, // hex_zero   — dim gray
    {200, 180, 60,  255}, // hex_ctrl   — yellow
    {80,  80,  160, 255}, // hex_space  — blue-gray
    {180, 60,  180, 255}, // hex_punct  — magenta
    {80,  200, 80,  255}, // hex_digit  — green
    {80,  120, 220, 255}, // hex_letter — blue
    {220, 100, 100, 255}, // hex_high   — red
    {80,  220, 80,  255}, // hex_full   — bright green
};

// Legacy single-entry name table kept for the settings UI
const char *theme_preset_names[THEME_PRESET_COUNT] = { "Dark" };
int         current_theme_preset = -1;

// apply named_themes[idx] as the active theme
void apply_theme_preset(int idx) {
    if (idx < 0 || idx >= named_theme_count) return;
    current_named_theme  = idx;
    current_theme_preset = idx;   // keep legacy var in sync for settings UI
    cfg.theme = named_themes[idx].colors;
}

// ---------------------------------------------------------------------------
// Shared path helper: resolves a sibling file next to the executable via
// /proc/self/exe; falls back to CWD filename if that fails.
// ---------------------------------------------------------------------------
static void get_sibling_path(const char *filename, char *out) {
    strncpy(out, filename, MAX_PATH - 1);
    out[MAX_PATH - 1] = '\0';
    char exe_buf[MAX_PATH];
    ssize_t len = readlink("/proc/self/exe", exe_buf, sizeof(exe_buf) - 1);
    if (len > 0) {
        exe_buf[len] = '\0';
        char *slash = strrchr(exe_buf, '/');
        if (slash) {
            *slash = '\0';
            snprintf(out, MAX_PATH, "%s/%s", exe_buf, filename);
        }
    }
}

// Convenience wrapper for the primary config file
static void get_config_path(char *out) { get_sibling_path("config.ini", out); }

// ---------------------------------------------------------------------------
// Helper: #RRGGBB or #RRGGBBAA → SDL_Color
// ---------------------------------------------------------------------------
static void parse_color(const char *val, SDL_Color *col) {
    if (val[0] != '#') return;
    unsigned long hex = strtoul(val + 1, NULL, 16);
    size_t len = strlen(val + 1);
    if (len <= 6) {
        col->r = (hex >> 16) & 0xFF;
        col->g = (hex >> 8)  & 0xFF;
        col->b =  hex        & 0xFF;
        col->a = 255;
    } else {
        col->r = (hex >> 24) & 0xFF;
        col->g = (hex >> 16) & 0xFF;
        col->b = (hex >> 8)  & 0xFF;
        col->a =  hex        & 0xFF;
    }
}


// ---------------------------------------------------------------------------
// Parse a color key=value pair into the given Theme struct.
// Returns true if the key was recognised.
// ---------------------------------------------------------------------------
static bool parse_theme_color(const char *k, const char *v, Theme *t) {
    if      (strcmp(k, "Bg")           == 0) { parse_color(v, &t->bg);           return true; }
    else if (strcmp(k, "AltBg")        == 0) { parse_color(v, &t->alt_bg);        return true; }
    else if (strcmp(k, "HeaderBg")     == 0) { parse_color(v, &t->header_bg);     return true; }
    else if (strcmp(k, "Text")         == 0) { parse_color(v, &t->text);          return true; }
    else if (strcmp(k, "TextDisabled") == 0) { parse_color(v, &t->text_disabled); return true; }
    else if (strcmp(k, "LinkText")     == 0) { parse_color(v, &t->link);          return true; }
    else if (strcmp(k, "HighlightBg")   == 0) { parse_color(v, &t->highlight_bg);   return true; }
    else if (strcmp(k, "HighlightText") == 0) { parse_color(v, &t->highlight_text); return true; }
    else if (strcmp(k, "MarkedText")    == 0) { parse_color(v, &t->marked);         return true; }
    else if (strcmp(k, "MenuBg")       == 0) { parse_color(v, &t->menu_bg);       return true; }
    else if (strcmp(k, "MenuBorder")   == 0) { parse_color(v, &t->menu_border);   return true; }
    // Hex-viewer byte-category colours (all optional)
    else if (strcmp(k, "HexZero")      == 0) { parse_color(v, &t->hex_zero);      return true; }
    else if (strcmp(k, "HexCtrl")      == 0) { parse_color(v, &t->hex_ctrl);      return true; }
    else if (strcmp(k, "HexSpace")     == 0) { parse_color(v, &t->hex_space);     return true; }
    else if (strcmp(k, "HexPunct")     == 0) { parse_color(v, &t->hex_punct);     return true; }
    else if (strcmp(k, "HexDigit")     == 0) { parse_color(v, &t->hex_digit);     return true; }
    else if (strcmp(k, "HexLetter")    == 0) { parse_color(v, &t->hex_letter);    return true; }
    else if (strcmp(k, "HexHigh")      == 0) { parse_color(v, &t->hex_high);      return true; }
    else if (strcmp(k, "HexFull")      == 0) { parse_color(v, &t->hex_full);      return true; }
    return false;
}

// ---------------------------------------------------------------------------
// Load named themes from an open FILE*.
// Sections named [Theme] (anonymous) or [Theme.SomeName] define presets.
// Each new section header starts a new named theme entry.
// Call with a NULL file to skip safely.
// ---------------------------------------------------------------------------
static void load_themes_from_file(FILE *f) {
    if (!f) return;

    char line[256];
    int  cur = -1;   // index into named_themes[] currently being filled

    while (fgets(line, sizeof(line), f)) {
        char *t = trim(line);
        if (t[0] == '\0' || t[0] == '#') continue;

        if (t[0] == '[') {
            // Section header
            char *close = strchr(t, ']');
            if (!close) continue;
            *close = '\0';
            char *section = t + 1;

            if (strncmp(section, "Theme", 5) == 0) {
                if (named_theme_count >= MAX_NAMED_THEMES) { cur = -1; continue; }

                // Derive display name first so we can check for duplicates
                char tname[MAX_THEME_NAME];
                if (section[5] == '.') {
                    strncpy(tname, section + 6, MAX_THEME_NAME - 1);
                    tname[MAX_THEME_NAME - 1] = '\0';
                } else {
                    strncpy(tname, "Theme", MAX_THEME_NAME - 1);
                }

                // Skip if a theme with this name was already loaded (e.g. from config.ini)
                bool dup = false;
                for (int i = 0; i < named_theme_count; i++) {
                    if (strcmp(named_themes[i].name, tname) == 0) { dup = true; break; }
                }
                if (dup) { cur = -1; continue; }

                cur = named_theme_count++;
                strncpy(named_themes[cur].name, tname, MAX_THEME_NAME - 1);
                named_themes[cur].name[MAX_THEME_NAME - 1] = '\0';
                // Initialise with dark fallback so partial definitions still work
                named_themes[cur].colors = dark_fallback;
            } else {
                cur = -1;  // non-theme section, stop filling
            }
            continue;
        }

        if (cur < 0) continue;

        char *eq = strchr(t, '=');
        if (!eq) continue;
        *eq = '\0';
        char *k = trim(t);
        char *v = trim(eq + 1);
        parse_theme_color(k, v, &named_themes[cur].colors);
    }
}

// ---------------------------------------------------------------------------
void load_config() {
    // ── Defaults ─────────────────────────────────────────────────────────────
    cfg.screen_w         = 0;   // 0 = not set; resolved after parsing
    cfg.screen_h         = 0;
    cfg.font_size_list   = 18;
    cfg.font_size_header = 14;
    cfg.font_size_footer = 14;
    cfg.font_size_menu   = 18;
    cfg.font_size_hex    = 16;   // slightly smaller default for hex viewer density

    strcpy(cfg.start_left,       "/");
    strcpy(cfg.start_right,      "/mnt/sdcard");
    cfg.gamecontrollerdb[0] = '\0';  // empty = look for gamecontrollerdb.txt next to exe
    cfg.font_path[0] = '\0';  // blank → scan_fonts() picks first from ./fonts/
    cfg.extra_image_ext_count = 0;
    cfg.extra_text_ext_count  = 0;
    cfg.show_hidden   = false;
    cfg.remember_dirs = false;
    cfg.exec_scripts  = false;
    cfg.single_pane   = false;
    cfg.rotation      = 0;

    // Default theme = built-in Dark fallback (overwritten below if file found)
    cfg.theme = dark_fallback;

    // Explorer / global keys
    cfg.k_confirm = SDL_CONTROLLER_BUTTON_A;
    cfg.k_back    = SDL_CONTROLLER_BUTTON_B;
    cfg.k_menu    = SDL_CONTROLLER_BUTTON_GUIDE;
    cfg.k_mark    = SDL_CONTROLLER_BUTTON_Y;
    cfg.k_pgup    = SDL_CONTROLLER_BUTTON_LEFTSHOULDER;
    cfg.k_pgdn    = SDL_CONTROLLER_BUTTON_RIGHTSHOULDER;

    // OSK keys
    cfg.osk_k_type   = SDL_CONTROLLER_BUTTON_A;
    cfg.osk_k_bksp   = SDL_CONTROLLER_BUTTON_X;
    cfg.osk_k_shift  = SDL_CONTROLLER_BUTTON_Y;
    cfg.osk_k_cancel = SDL_CONTROLLER_BUTTON_B;
    cfg.osk_k_toggle = SDL_CONTROLLER_BUTTON_BACK;
    cfg.osk_k_ins    = SDL_CONTROLLER_BUTTON_START;

    // ── Locate config file ────────────────────────────────────────────────────
    char config_path[MAX_PATH];
    get_config_path(config_path);

    // ── First pass: load themes from config.ini ───────────────────────────────
    named_theme_count   = 0;
    current_named_theme = -1;

    {
        FILE *tf = fopen(config_path, "r");
        load_themes_from_file(tf);
        if (tf) fclose(tf);
    }

    // ── Second: look for a sibling theme.ini and append its themes ─────────────
    {
        char theme_path[MAX_PATH];
        get_sibling_path("theme.ini", theme_path);
        FILE *tf = fopen(theme_path, "r");
        if (tf) {
            load_themes_from_file(tf);
            fclose(tf);
        }
    }

    // If no file-based themes were found, seed with the built-in Dark fallback
    if (named_theme_count == 0) {
        named_theme_count = 1;
        strncpy(named_themes[0].name, "Dark", MAX_THEME_NAME - 1);
        named_themes[0].colors = dark_fallback;
    }

    // ── Main config.ini parse (non-theme keys + legacy inline [Theme]) ─────────
    FILE *f = fopen(config_path, "r");
    if (!f) {
        // No config file — apply first available theme and return
        apply_theme_preset(0);
        return;
    }

    char line[256];
    char cur_section[64] = "";
    bool in_inline_theme = false;  // true when we're in [Theme] / [Theme.X] section

    while (fgets(line, sizeof(line), f)) {
        char *t = trim(line);
        if (t[0] == '\0' || t[0] == '#') continue;

        if (t[0] == '[') {
            char *close = strchr(t, ']');
            if (close) { *close = '\0'; strncpy(cur_section, t + 1, 63); cur_section[63] = '\0'; }
            // If we hit a [Theme…] section, mark that we should not re-parse it here
            // (already loaded by load_themes_from_file above).
            in_inline_theme = (strncmp(cur_section, "Theme", 5) == 0);
            continue;
        }

        // Skip theme color lines — handled already
        if (in_inline_theme) continue;

        char *eq = strchr(t, '=');
        if (!eq) continue;
        *eq = '\0';
        char *k = trim(t);
        char *v = trim(eq + 1);

        // [Display]
        if      (strcmp(k, "ScreenWidth")      == 0) { int v2 = atoi(v); if (v2 > 0) cfg.screen_w = v2; }
        else if (strcmp(k, "ScreenHeight")     == 0) { int v2 = atoi(v); if (v2 > 0) cfg.screen_h = v2; }
        else if (strcmp(k, "FontSizeList")     == 0) cfg.font_size_list   = atoi(v);
        else if (strcmp(k, "FontSizeHeader")   == 0) cfg.font_size_header = atoi(v);
        else if (strcmp(k, "FontSizeFooter")   == 0) cfg.font_size_footer = atoi(v);
        else if (strcmp(k, "FontSizeMenu")     == 0) cfg.font_size_menu   = atoi(v);
        else if (strcmp(k, "FontSizeHex")      == 0) cfg.font_size_hex    = atoi(v);
        else if (strcmp(k, "FontFile")         == 0) strncpy(cfg.font_path, v, MAX_PATH - 1);
        // [General]
        else if (strcmp(k, "ShowHidden")   == 0) cfg.show_hidden   = (strcmp(v,"true")==0 || strcmp(v,"1")==0);
        else if (strcmp(k, "RememberDirs") == 0) cfg.remember_dirs = (strcmp(v,"true")==0 || strcmp(v,"1")==0);
        else if (strcmp(k, "ExecScripts")  == 0) cfg.exec_scripts  = (strcmp(v,"true")==0 || strcmp(v,"1")==0);
        else if (strcmp(k, "SinglePane")   == 0) cfg.single_pane   = (strcmp(v,"true")==0 || strcmp(v,"1")==0);
        else if (strcmp(k, "Rotation")     == 0) { int r = atoi(v); cfg.rotation = (r >= 0 && r <= 3) ? r : 0; }
        // [Paths]
        else if (strcmp(k, "StartDirectoryLeft")  == 0) strcpy(cfg.start_left,        v);
        else if (strcmp(k, "StartDirectoryRight") == 0) strcpy(cfg.start_right,       v);
        else if (strcmp(k, "GameControllerDB")    == 0) strncpy(cfg.gamecontrollerdb, v, MAX_PATH - 1);
        // [Keys]
        else if (strcmp(k, "KeyConfirm") == 0) cfg.k_confirm = SDL_GameControllerGetButtonFromString(v);
        else if (strcmp(k, "KeyBack")    == 0) cfg.k_back    = SDL_GameControllerGetButtonFromString(v);
        else if (strcmp(k, "KeyMenu")    == 0) cfg.k_menu    = SDL_GameControllerGetButtonFromString(v);
        else if (strcmp(k, "KeyMark")    == 0) cfg.k_mark    = SDL_GameControllerGetButtonFromString(v);
        else if (strcmp(k, "KeyPgUp")    == 0) cfg.k_pgup    = SDL_GameControllerGetButtonFromString(v);
        else if (strcmp(k, "KeyPgDn")    == 0) cfg.k_pgdn    = SDL_GameControllerGetButtonFromString(v);
        // [OskKeys]
        else if (strcmp(k, "OskKeyType")   == 0) cfg.osk_k_type   = SDL_GameControllerGetButtonFromString(v);
        else if (strcmp(k, "OskKeyBksp")   == 0) cfg.osk_k_bksp   = SDL_GameControllerGetButtonFromString(v);
        else if (strcmp(k, "OskKeyShift")  == 0) cfg.osk_k_shift  = SDL_GameControllerGetButtonFromString(v);
        else if (strcmp(k, "OskKeyCancel") == 0) cfg.osk_k_cancel = SDL_GameControllerGetButtonFromString(v);
        else if (strcmp(k, "OskKeyToggle") == 0) cfg.osk_k_toggle = SDL_GameControllerGetButtonFromString(v);
        else if (strcmp(k, "OskKeyIns")    == 0) cfg.osk_k_ins    = SDL_GameControllerGetButtonFromString(v);
        // [ActiveTheme] — name of the theme to activate on startup
        else if (strcmp(k, "ActiveTheme") == 0) {
            for (int i = 0; i < named_theme_count; i++) {
                if (strcmp(named_themes[i].name, v) == 0) {
                    apply_theme_preset(i);
                    break;
                }
            }
        }
        // [FileTypes] — extra extensions appended to built-in lists
        // Values are space and/or comma separated; leading dot is optional.
        else if (strcmp(k, "ExtraImageExts") == 0 || strcmp(k, "ExtraTextExts") == 0) {
            bool is_img = (strcmp(k, "ExtraImageExts") == 0);
            int  *cnt   = is_img ? &cfg.extra_image_ext_count : &cfg.extra_text_ext_count;
            // Tokenise on spaces and commas
            char copy[512]; strncpy(copy, v, 511); copy[511] = '\0';
            char *tok = strtok(copy, " ,\t");
            while (tok && *cnt < MAX_EXTRA_EXTS) {
                char ext[MAX_EXT_LEN];
                int ei = 0;
                // Ensure leading dot
                if (tok[0] != '.') ext[ei++] = '.';
                while (*tok && ei < MAX_EXT_LEN - 1)
                    ext[ei++] = (char)tolower((unsigned char)*tok++);
                ext[ei] = '\0';
                if (is_img)
                    strncpy(cfg.extra_image_exts[*cnt], ext, MAX_EXT_LEN - 1);
                else
                    strncpy(cfg.extra_text_exts[*cnt],  ext, MAX_EXT_LEN - 1);
                (*cnt)++;
                tok = strtok(NULL, " ,\t");
            }
        }
    }
    fclose(f);

    // If no [ActiveTheme] line matched, apply the first available theme
    if (current_named_theme < 0 && named_theme_count > 0)
        apply_theme_preset(0);

    // Resolve screen size — use display resolution on first launch (no saved value)
    if (cfg.screen_w <= 0 || cfg.screen_h <= 0) {
        SDL_DisplayMode dm;
        if (SDL_GetCurrentDisplayMode(0, &dm) == 0 && dm.w > 0 && dm.h > 0) {
            cfg.screen_w = dm.w;
            cfg.screen_h = dm.h;
        } else {
            cfg.screen_w = 640;   // safe fallback if SDL can't query the display
            cfg.screen_h = 480;
        }
    }
}

// ---------------------------------------------------------------------------
void save_config() {
    char config_path[MAX_PATH];
    get_config_path(config_path);
    FILE *f = fopen(config_path, "w");
    if (!f) return;

    fprintf(f, "[General]\n");
    fprintf(f, "ShowHidden=%s\n",   cfg.show_hidden   ? "true" : "false");
    fprintf(f, "RememberDirs=%s\n", cfg.remember_dirs ? "true" : "false");
    fprintf(f, "ExecScripts=%s\n",  cfg.exec_scripts  ? "true" : "false");
    fprintf(f, "SinglePane=%s\n",   cfg.single_pane   ? "true" : "false");

    fprintf(f, "\n[Display]\n");
    // Always save physical dims so that Rotation= can be applied correctly on next load
    int save_w = (cfg.rotation == 1 || cfg.rotation == 3) ? cfg.screen_h : cfg.screen_w;
    int save_h = (cfg.rotation == 1 || cfg.rotation == 3) ? cfg.screen_w : cfg.screen_h;
    fprintf(f, "ScreenWidth=%d\n",   save_w);
    fprintf(f, "ScreenHeight=%d\n",  save_h);
    fprintf(f, "Rotation=%d\n",      cfg.rotation);
    fprintf(f, "FontSizeList=%d\n",  cfg.font_size_list);
    fprintf(f, "FontSizeHeader=%d\n",cfg.font_size_header);
    fprintf(f, "FontSizeFooter=%d\n",cfg.font_size_footer);
    fprintf(f, "FontSizeMenu=%d\n",  cfg.font_size_menu);
    fprintf(f, "FontSizeHex=%d\n",   cfg.font_size_hex);
    fprintf(f, "FontFile=%s\n",      cfg.font_path);

    fprintf(f, "\n[Paths]\n");
    fprintf(f, "StartDirectoryLeft=%s\n",  cfg.start_left);
    fprintf(f, "StartDirectoryRight=%s\n", cfg.start_right);
    fprintf(f, "# Path to gamecontrollerdb.txt; leave blank to search next to the executable.\n");
    fprintf(f, "GameControllerDB=%s\n", cfg.gamecontrollerdb);

    fprintf(f, "\n[Keys]\n");
    fprintf(f, "# SDL GameController button names: a, b, x, y, back, start, leftshoulder, rightshoulder, guide\n");
    const char *s;
    s = SDL_GameControllerGetStringForButton(cfg.k_confirm); fprintf(f, "KeyConfirm=%s\n", s ? s : "a");
    s = SDL_GameControllerGetStringForButton(cfg.k_back);    fprintf(f, "KeyBack=%s\n",    s ? s : "b");
    s = SDL_GameControllerGetStringForButton(cfg.k_menu);    fprintf(f, "KeyMenu=%s\n",    s ? s : "guide");
    s = SDL_GameControllerGetStringForButton(cfg.k_mark);    fprintf(f, "KeyMark=%s\n",    s ? s : "y");
    s = SDL_GameControllerGetStringForButton(cfg.k_pgup);    fprintf(f, "KeyPgUp=%s\n",    s ? s : "leftshoulder");
    s = SDL_GameControllerGetStringForButton(cfg.k_pgdn);    fprintf(f, "KeyPgDn=%s\n",    s ? s : "rightshoulder");

    fprintf(f, "\n[OskKeys]\n");
    fprintf(f, "# On-screen keyboard keys — independent from explorer keys, fully rebindable.\n");
    s = SDL_GameControllerGetStringForButton(cfg.osk_k_type);   fprintf(f, "OskKeyType=%s\n",   s ? s : "a");
    s = SDL_GameControllerGetStringForButton(cfg.osk_k_bksp);   fprintf(f, "OskKeyBksp=%s\n",   s ? s : "x");
    s = SDL_GameControllerGetStringForButton(cfg.osk_k_shift);  fprintf(f, "OskKeyShift=%s\n",  s ? s : "y");
    s = SDL_GameControllerGetStringForButton(cfg.osk_k_cancel); fprintf(f, "OskKeyCancel=%s\n", s ? s : "b");
    s = SDL_GameControllerGetStringForButton(cfg.osk_k_toggle); fprintf(f, "OskKeyToggle=%s\n", s ? s : "back");
    s = SDL_GameControllerGetStringForButton(cfg.osk_k_ins);    fprintf(f, "OskKeyIns=%s\n",    s ? s : "leftshoulder");

    // Write active theme name so it's restored on next launch
    fprintf(f, "\n[ActiveTheme]\n");
    if (current_named_theme >= 0 && current_named_theme < named_theme_count)
        fprintf(f, "ActiveTheme=%s\n", named_themes[current_named_theme].name);
    else
        fprintf(f, "# ActiveTheme=Dark\n");

    // Write extra extension lists (only if non-empty, to keep the file clean)
    fprintf(f, "\n[FileTypes]\n");
    fprintf(f, "# Extra extensions appended to the built-in image/text lists.\n");
    fprintf(f, "# Space or comma separated, leading dot optional. Example: .raw .exr\n");
    fprintf(f, "ExtraImageExts=");
    for (int i = 0; i < cfg.extra_image_ext_count; i++)
        fprintf(f, "%s%s", i ? " " : "", cfg.extra_image_exts[i]);
    fprintf(f, "\n");
    fprintf(f, "ExtraTextExts=");
    for (int i = 0; i < cfg.extra_text_ext_count; i++)
        fprintf(f, "%s%s", i ? " " : "", cfg.extra_text_exts[i]);
    fprintf(f, "\n");

    fclose(f);
}
