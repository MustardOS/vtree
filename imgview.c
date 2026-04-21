// imgview.c — image viewer for vTree
// -----------------------------------------------------------------------------
// Layout:
//   header bar  — filename  |  WxH px  |  zoom %
//   image area  — centred, fit-to-screen by default, pannable when zoomed
//   footer bar  — key hints
//
// Controls (actual buttons depend on key bindings in config.ini):
//   D-pad          pan image (when zoomed beyond fit)
//   L1 / R1        previous / next image in directory
//   Y              zoom in  (+25% per press, max 800%)
//   X              zoom out (−25% per press, min 25%)
//   k_confirm      toggle 1:1 (actual pixels) vs fit-to-screen
//   k_mark         reset to fit-to-screen
//   k_back         close, return to explorer
// -----------------------------------------------------------------------------

#include "vtree.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <dirent.h>

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
typedef struct {
    char        path[MAX_PATH];
    char        dir[MAX_PATH];      // directory containing the file
    SDL_Texture *tex;
    int         img_w, img_h;       // original image dimensions in pixels
    bool        load_failed;

    // Zoom — stored as a rational: zoom_num/zoom_den.
    // At fit-to-screen both values are set to match the fit factor.
    // We track a float for simplicity and snap to nice percentages.
    float       zoom;               // current zoom factor (1.0 = 100%)
    float       fit_zoom;           // zoom that makes image fill the view

    // Pan offsets (pixels in image space, positive = panned right/down)
    int         pan_x, pan_y;

    bool        at_fit;             // true when zoom == fit_zoom (reset state)

    // Sibling image navigation
    char        siblings[MAX_FILES][256];  // basenames of image files in dir
    int         sibling_count;
    int         sibling_idx;        // index of current file in siblings[]
} ImgViewState;

static ImgViewState iv;

// ---------------------------------------------------------------------------
// Known image extensions — must match is_image_file() in main.c
// ---------------------------------------------------------------------------
static bool iv_is_image(const char *name) {
    static const char *exts[] = {
        ".png",".jpg",".jpeg",".bmp",".gif",".tga",
        ".tiff",".tif",".webp",".lbm",".pnm",".pbm",".pgm",".ppm",".xcf",".xpm",
        NULL
    };
    const char *dot = strrchr(name, '.');
    if (!dot) return false;
    char lo[16]; int i = 0;
    while (dot[i] && i < 15) { lo[i] = (char)tolower((unsigned char)dot[i]); i++; }
    lo[i] = '\0';
    for (int j = 0; exts[j]; j++) if (strcmp(lo, exts[j]) == 0) return true;
    return false;
}

// ---------------------------------------------------------------------------
// Geometry helpers
// ---------------------------------------------------------------------------
static int iv_head_h(void) { return cfg.font_size_header + 12; }
static int iv_foot_h(void) { return cfg.font_size_footer + 16; }
static int iv_view_w(void) { return cfg.screen_w; }
static int iv_view_h(void) { return cfg.screen_h - iv_head_h() - iv_foot_h(); }

// Recalculate fit_zoom from current image and view dimensions
static void iv_calc_fit(void) {
    if (iv.img_w <= 0 || iv.img_h <= 0) { iv.fit_zoom = 1.0f; return; }
    float zx = (float)iv_view_w() / (float)iv.img_w;
    float zy = (float)iv_view_h() / (float)iv.img_h;
    iv.fit_zoom = (zx < zy) ? zx : zy;
    // Never upscale tiny images beyond 1:1 in fit mode — keeps pixels sharp
    if (iv.fit_zoom > 1.0f) iv.fit_zoom = 1.0f;
}

// Reset to fit-to-screen
static void iv_reset_fit(void) {
    iv_calc_fit();
    iv.zoom   = iv.fit_zoom;
    iv.pan_x  = 0;
    iv.pan_y  = 0;
    iv.at_fit = true;
}

// Clamp pan so the image never drifts entirely off-screen
static void iv_clamp_pan(void) {
    int dw = iv_view_w();
    int dh = iv_view_h();
    int sw = (int)((float)iv.img_w * iv.zoom);
    int sh = (int)((float)iv.img_h * iv.zoom);

    // Maximum pan: half the excess beyond the view (centred = 0)
    int max_px = sw > dw ? (sw - dw) / 2 + dw / 4 : 0;
    int max_py = sh > dh ? (sh - dh) / 2 + dh / 4 : 0;

    if (iv.pan_x < -max_px) iv.pan_x = -max_px;
    if (iv.pan_x >  max_px) iv.pan_x =  max_px;
    if (iv.pan_y < -max_py) iv.pan_y = -max_py;
    if (iv.pan_y >  max_py) iv.pan_y =  max_py;
}

// ---------------------------------------------------------------------------
// Sibling scan — collect image files in the same directory, sorted
// ---------------------------------------------------------------------------
static int iv_name_cmp(const void *a, const void *b) {
    return strcasecmp((const char *)a, (const char *)b);
}

static void iv_scan_siblings(const char *dir, const char *current_name) {
    iv.sibling_count = 0;
    iv.sibling_idx   = 0;

    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) && iv.sibling_count < MAX_FILES) {
        if (e->d_name[0] == '.') continue;
        if (iv_is_image(e->d_name)) {
            strncpy(iv.siblings[iv.sibling_count], e->d_name, 255);
            iv.siblings[iv.sibling_count][255] = '\0';
            iv.sibling_count++;
        }
    }
    closedir(d);

    // Sort alphabetically
    qsort(iv.siblings, iv.sibling_count, 256, iv_name_cmp);

    // Find current file
    for (int i = 0; i < iv.sibling_count; i++) {
        if (strcasecmp(iv.siblings[i], current_name) == 0) {
            iv.sibling_idx = i;
            break;
        }
    }
}

// ---------------------------------------------------------------------------
// Load texture from path, update state
// ---------------------------------------------------------------------------
static void iv_load(const char *path) {
    if (iv.tex) { SDL_DestroyTexture(iv.tex); iv.tex = NULL; }
    iv.img_w = iv.img_h = 0;
    iv.load_failed = false;

    SDL_Surface *surf = IMG_Load(path);
    if (!surf) {
        vtree_log("[imgview] IMG_Load FAILED: %s (%s)\n", path, IMG_GetError());
        iv.load_failed = true; return;
    }

    iv.img_w = surf->w;
    iv.img_h = surf->h;
    iv.tex   = SDL_CreateTextureFromSurface(renderer, surf);
    SDL_FreeSurface(surf);

    if (!iv.tex) { iv.load_failed = true; return; }
    iv_reset_fit();
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
void imgview_open(const char *path) {
    memset(&iv, 0, sizeof(iv));
    strncpy(iv.path, path, MAX_PATH - 1);

    // Split into dir + basename for sibling navigation
    const char *slash = strrchr(path, '/');
    if (slash) {
        int dlen = (int)(slash - path);
        if (dlen == 0) { iv.dir[0] = '/'; iv.dir[1] = '\0'; }
        else { strncpy(iv.dir, path, dlen); iv.dir[dlen] = '\0'; }
        iv_scan_siblings(iv.dir, slash + 1);
    } else {
        strcpy(iv.dir, ".");
        iv_scan_siblings(".", path);
    }

    iv_load(path);
    vtree_log("[imgview] opened: %s  %dx%d  siblings=%d  zoom=fit%s\n",
              path, iv.img_w, iv.img_h, iv.sibling_count,
              iv.load_failed ? "  [LOAD FAILED]" : "");
    current_mode = MODE_VIEW_IMAGE;
}

void imgview_close(void) {
    if (iv.tex) { SDL_DestroyTexture(iv.tex); iv.tex = NULL; }
}

// Navigate to sibling by delta (-1 = prev, +1 = next)
static void iv_nav_sibling(int delta) {
    if (iv.sibling_count < 2) return;
    iv.sibling_idx = (iv.sibling_idx + delta + iv.sibling_count) % iv.sibling_count;
    char new_path[MAX_PATH];
    join_path(new_path, iv.dir, iv.siblings[iv.sibling_idx]);
    strncpy(iv.path, new_path, MAX_PATH - 1);
    iv_load(new_path);   // resets zoom/pan via iv_reset_fit
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------
#define IV_PAN_STEP   24    // pixels per d-pad repeat at 1× zoom
#define IV_ZOOM_STEP  0.25f // zoom increment per L1/R1 press
#define IV_ZOOM_MIN   0.25f
#define IV_ZOOM_MAX   8.0f

void imgview_handle_button(SDL_GameControllerButton btn, Uint32 now) {
    (void)now;

    if (btn == cfg.k_back) {
        current_mode = MODE_EXPLORER;
        return;
    }

    // Y — reset to fit
    if (btn == cfg.k_mark) {
        iv_reset_fit();
        return;
    }

    // A — toggle 1:1 vs fit
    if (btn == cfg.k_confirm) {
        if (iv.at_fit) {
            iv.zoom   = 1.0f;
            iv.pan_x  = 0;
            iv.pan_y  = 0;
            iv.at_fit = false;
        } else {
            iv_reset_fit();
        }
        return;
    }

    // Y — zoom in
    if (btn == SDL_CONTROLLER_BUTTON_Y) {
        float nz = iv.zoom + IV_ZOOM_STEP;
        if (nz > IV_ZOOM_MAX) nz = IV_ZOOM_MAX;
        iv.zoom   = nz;
        iv.at_fit = false;
        iv_clamp_pan();
        return;
    }

    // X — zoom out
    if (btn == SDL_CONTROLLER_BUTTON_X) {
        float nz = iv.zoom - IV_ZOOM_STEP;
        if (nz < IV_ZOOM_MIN) nz = IV_ZOOM_MIN;
        iv.zoom   = nz;
        iv.at_fit = (iv.zoom == iv.fit_zoom);
        iv_clamp_pan();
        return;
    }

    // L1 / R1 — prev / next image
    if (btn == SDL_CONTROLLER_BUTTON_LEFTSHOULDER) {
        if (iv.sibling_count > 1) iv_nav_sibling(-1);
        return;
    }
    if (btn == SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) {
        if (iv.sibling_count > 1) iv_nav_sibling(+1);
        return;
    }

    // D-pad — pan (step scales with zoom so it feels consistent)
    int step = (int)((float)IV_PAN_STEP / iv.zoom);
    if (step < 4) step = 4;

    int old_x = iv.pan_x, old_y = iv.pan_y;
    if (btn == SDL_CONTROLLER_BUTTON_DPAD_UP)    iv.pan_y += step;
    if (btn == SDL_CONTROLLER_BUTTON_DPAD_DOWN)  iv.pan_y -= step;
    if (btn == SDL_CONTROLLER_BUTTON_DPAD_LEFT)  iv.pan_x += step;
    if (btn == SDL_CONTROLLER_BUTTON_DPAD_RIGHT) iv.pan_x -= step;
    iv_clamp_pan();
    if (iv.pan_x != old_x || iv.pan_y != old_y) iv.at_fit = false;
}

// ---------------------------------------------------------------------------
// Render
// ---------------------------------------------------------------------------
void imgview_draw(void) {
    SDL_SetRenderDrawColor(renderer,
        cfg.theme.bg.r, cfg.theme.bg.g, cfg.theme.bg.b, 255);
    SDL_RenderClear(renderer);

    int head_h = iv_head_h();
    int foot_h = iv_foot_h();
    int view_y = head_h;
    int vw     = iv_view_w();
    int vh     = iv_view_h();

    // ── Header ───────────────────────────────────────────────────────────────
    SDL_SetRenderDrawColor(renderer,
        cfg.theme.header_bg.r, cfg.theme.header_bg.g, cfg.theme.header_bg.b, 255);
    SDL_Rect hr = {0, 0, cfg.screen_w, head_h};
    SDL_RenderFillRect(renderer, &hr);

    // Basename
    const char *bname = strrchr(iv.path, '/');
    bname = bname ? bname + 1 : iv.path;
    draw_txt(font_header, bname, 6, (head_h - cfg.font_size_header) / 2 - 1,
             cfg.theme.text);

    // Right side: dimensions + zoom
    if (!iv.load_failed && iv.img_w > 0) {
        char info[64];
        int zoom_pct = (int)(iv.zoom * 100.0f + 0.5f);
        snprintf(info, sizeof(info), "%dx%d  %d%%", iv.img_w, iv.img_h, zoom_pct);
        int iw = 0;
        if (font_header) TTF_SizeText(font_header, info, &iw, NULL);
        draw_txt(font_header, info, cfg.screen_w - iw - 6,
                 (head_h - cfg.font_size_header) / 2 - 1, cfg.theme.marked);
    }

    // Sibling counter (e.g. "3 / 12") centred in header when multiple images present
    if (iv.sibling_count > 1) {
        char nav[32];
        snprintf(nav, sizeof(nav), "%d / %d", iv.sibling_idx + 1, iv.sibling_count);
        int nw = 0;
        if (font_header) TTF_SizeText(font_header, nav, &nw, NULL);
        draw_txt(font_header, nav, (cfg.screen_w - nw) / 2,
                 (head_h - cfg.font_size_header) / 2 - 1, cfg.theme.text_disabled);
    }

    // ── Image area ────────────────────────────────────────────────────────────
    if (iv.load_failed || !iv.tex) {
        const char *msg = iv.load_failed ? tr("ImgView_LoadFailed") : tr("ImgView_NoImage");
        int mw = 0;
        if (font_list) TTF_SizeText(font_list, msg, &mw, NULL);
        draw_txt(font_list, msg,
                 (cfg.screen_w - mw) / 2,
                 view_y + (vh - cfg.font_size_list) / 2,
                 cfg.theme.text_disabled);
    } else {
        // Compute scaled dimensions
        int sw = (int)((float)iv.img_w * iv.zoom);
        int sh = (int)((float)iv.img_h * iv.zoom);

        // Centre in view, then apply pan
        int dx = (vw - sw) / 2 + iv.pan_x;
        int dy = view_y + (vh - sh) / 2 + iv.pan_y;

        SDL_Rect dst = { dx, dy, sw, sh };

        // Clip to image area so it doesn't bleed into header/footer
        SDL_Rect clip = { 0, view_y, vw, vh };
        SDL_RenderSetClipRect(renderer, &clip);
        SDL_RenderCopy(renderer, iv.tex, NULL, &dst);
        SDL_RenderSetClipRect(renderer, NULL);

        // Thin border around image when smaller than view (helps on dark images)
        if (sw < vw || sh < vh) {
            SDL_SetRenderDrawColor(renderer,
                cfg.theme.text_disabled.r, cfg.theme.text_disabled.g,
                cfg.theme.text_disabled.b, 80);
            SDL_Rect border = { dx - 1, dy - 1, sw + 2, sh + 2 };
            SDL_RenderDrawRect(renderer, &border);
        }
    }

    // ── Footer ────────────────────────────────────────────────────────────────
    SDL_SetRenderDrawColor(renderer,
        cfg.theme.header_bg.r, cfg.theme.header_bg.g, cfg.theme.header_bg.b, 255);
    SDL_Rect fr = { 0, cfg.screen_h - foot_h, cfg.screen_w, foot_h };
    SDL_RenderFillRect(renderer, &fr);

    char hint[256];
    const char *lb_prev    = btn_label(cfg.k_pgup);
    const char *lb_next    = btn_label(cfg.k_pgdn);
    const char *lb_zoomin  = btn_label(cfg.k_menu);              // Y by default; zoom in
    const char *lb_zoomout = btn_label(SDL_CONTROLLER_BUTTON_X); // X; zoom out
    const char *lb_confirm = btn_label(cfg.k_confirm);
    const char *lb_fit     = btn_label(cfg.k_mark);
    const char *lb_close   = btn_label(cfg.k_back);
    if (iv.at_fit && iv.sibling_count > 1) {
        snprintf(hint, sizeof(hint), tr("ImgView_HintMulti"), lb_prev, lb_next, lb_zoomin, lb_zoomout, lb_confirm, lb_fit, lb_close);
    } else if (iv.at_fit) {
        snprintf(hint, sizeof(hint), tr("ImgView_HintFit"), lb_zoomin, lb_zoomout, lb_confirm, lb_fit, lb_close);
    } else {
        int zoom_pct = (int)(iv.zoom * 100.0f + 0.5f);
        snprintf(hint, sizeof(hint), tr("ImgView_HintZoom"), lb_prev, lb_next, lb_zoomin, lb_zoomout, zoom_pct, lb_confirm, lb_fit, lb_close);
    }
    draw_txt_clipped(font_footer, hint, 8,
             cfg.screen_h - foot_h + (foot_h - cfg.font_size_footer) / 2,
             cfg.screen_w - 16, cfg.theme.text);
}
