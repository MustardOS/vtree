// snake.c — Easter egg snake game
// Trigger: About screen, hold R1 + DpadUp + press A
#include "vtree.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------
#define SNAKE_CELL      16
#define SNAKE_HDR_H     36
#define SNAKE_MAX       2048
#define SNAKE_HS_MAX    5       // top scores kept
#define SNAKE_AUDIO_SR  22050   // sample rate

// Character set for name entry: A-Z, 0-9, space
#define SNAKE_CHARS "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 "
#define SNAKE_CHAR_COUNT 37

// ---------------------------------------------------------------------------
// Types
// ---------------------------------------------------------------------------
typedef enum { DIR_RIGHT, DIR_LEFT, DIR_UP, DIR_DOWN } SnakeDir;
typedef struct { int x, y; } Cell;
typedef struct { char name[4]; int score; } HighScore;

// ---------------------------------------------------------------------------
// Theme
// ---------------------------------------------------------------------------
typedef struct {
    SDL_Color bg, header_bg, grid, snake_head, snake_body_hi, snake_body_lo;
    SDL_Color food, food_pip, text_hi, text_mid, text_dim, gameover, header_text;
} SnakeTheme;

static bool snake_green_mode = true;  // true = Nokia 3310 classic, false = system theme

static SnakeTheme current_theme(void) {
    SnakeTheme t;
    if (snake_green_mode) {
        // Nokia 3310 LCD palette: muted gray-green, desaturated ink
        t.bg           = (SDL_Color){172, 185, 148, 255};  // pale gray-green screen
        t.header_bg    = (SDL_Color){ 78,  92,  62, 255};  // dark gray-green band
        t.grid         = (SDL_Color){158, 171, 134, 255};  // grid lines, slightly darker than bg
        t.snake_head   = (SDL_Color){ 42,  54,  32, 255};  // dark gray-green ink — head
        t.snake_body_hi= (SDL_Color){ 78,  92,  62, 255};  // mid ink — body highlight
        t.snake_body_lo= (SDL_Color){ 42,  54,  32, 255};  // dark ink — body shadow
        t.food         = (SDL_Color){ 42,  54,  32, 255};  // solid dark dot
        t.food_pip     = (SDL_Color){ 78,  92,  62, 255};  // pip highlight
        t.text_hi      = (SDL_Color){ 42,  54,  32, 255};  // darkest — headings
        t.text_mid     = (SDL_Color){ 78,  92,  62, 255};  // mid — score
        t.text_dim     = (SDL_Color){108, 120,  90, 255};  // dim — hints
        t.gameover     = (SDL_Color){ 42,  54,  32, 255};  // game over text
        t.header_text  = (SDL_Color){172, 185, 148, 255};  // light on dark band
    } else {
        // Use cfg.theme colours
        t.bg           = cfg.theme.bg;
        t.header_bg    = cfg.theme.header_bg;
        t.grid         = cfg.theme.alt_bg;
        t.snake_head   = cfg.theme.highlight_text;
        t.snake_body_hi= cfg.theme.highlight_bg;
        t.snake_body_lo= cfg.theme.text_disabled;
        t.food         = cfg.theme.marked;
        t.food_pip     = cfg.theme.highlight_text;
        t.text_hi      = cfg.theme.text;
        t.text_mid     = cfg.theme.text;
        t.text_dim     = cfg.theme.text_disabled;
        t.gameover     = cfg.theme.marked;
        t.header_text  = cfg.theme.text;
    }
    return t;
}

// ---------------------------------------------------------------------------
// High scores
// ---------------------------------------------------------------------------
static HighScore high_scores[SNAKE_HS_MAX];
static int       hs_count = 0;

static void hs_dat_path(char *out) {
    if (vtree_exe_dir[0])
        snprintf(out, MAX_PATH, "%s/snake.dat", vtree_exe_dir);
    else
        snprintf(out, MAX_PATH, "snake.dat");
}

static void hs_load(void) {
    char path[MAX_PATH]; hs_dat_path(path);
    hs_count = 0;
    FILE *f = fopen(path, "r");
    if (!f) return;
    char name[8]; int score;
    while (hs_count < SNAKE_HS_MAX && fscanf(f, "%7s %d\n", name, &score) == 2) {
        strncpy(high_scores[hs_count].name, name, 3);
        high_scores[hs_count].name[3] = '\0';
        high_scores[hs_count].score   = score;
        hs_count++;
    }
    fclose(f);
}

static void hs_save(void) {
    char path[MAX_PATH]; hs_dat_path(path);
    FILE *f = fopen(path, "w");
    if (!f) return;
    for (int i = 0; i < hs_count; i++)
        fprintf(f, "%s %d\n", high_scores[i].name, high_scores[i].score);
    fclose(f);
}

// Returns insertion rank (0-based) if score qualifies, -1 otherwise
static int hs_qualifies(int score) {
    if (score <= 0) return -1;
    for (int i = 0; i < SNAKE_HS_MAX; i++) {
        if (i >= hs_count || score > high_scores[i].score) return i;
    }
    return -1;
}

static void hs_insert(int rank, const char *name, int score) {
    int end = hs_count < SNAKE_HS_MAX ? hs_count : SNAKE_HS_MAX - 1;
    for (int i = end; i > rank; i--) high_scores[i] = high_scores[i - 1];
    strncpy(high_scores[rank].name, name, 3);
    high_scores[rank].name[3] = '\0';
    high_scores[rank].score = score;
    if (hs_count < SNAKE_HS_MAX) hs_count++;
}

// ---------------------------------------------------------------------------
// Audio
// ---------------------------------------------------------------------------
static SDL_AudioDeviceID snake_audio = 0;

static void audio_open(void) {
    if (snake_audio) return;
    SDL_AudioSpec want = {0};
    want.freq     = SNAKE_AUDIO_SR;
    want.format   = AUDIO_S16SYS;
    want.channels = 1;
    want.samples  = 512;
    snake_audio = SDL_OpenAudioDevice(NULL, 0, &want, NULL, 0);
    if (snake_audio) SDL_PauseAudioDevice(snake_audio, 0);
}

static void audio_close(void) {
    if (snake_audio) { SDL_CloseAudioDevice(snake_audio); snake_audio = 0; }
}

// Square-wave tone with optional frequency sweep and fade-out
static void play_tone(int f0, int f1, int ms, float vol) {
    if (!snake_audio) return;
    int n = SNAKE_AUDIO_SR * ms / 1000;
    if (n <= 0) return;
    Sint16 *buf = (Sint16 *)malloc((size_t)n * sizeof(Sint16));
    if (!buf) return;
    double phase = 0.0;
    for (int i = 0; i < n; i++) {
        double t    = (double)i / n;
        double freq = f0 + t * (f1 - f0);
        if (freq < 1.0) freq = 1.0;
        phase += freq / SNAKE_AUDIO_SR;
        double frac = phase - (int)phase;
        double env  = 1.0 - t * 0.85;
        Sint16 s    = (Sint16)(32767.0 * vol * env * (frac < 0.5 ? 1.0 : -1.0));
        buf[i] = s;
    }
    SDL_QueueAudio(snake_audio, buf, (Uint32)n * sizeof(Sint16));
    free(buf);
}

static void sound_eat(void)   { play_tone(440, 880, 70,  0.25f); }
static void sound_die(void)   { play_tone(440, 80,  350, 0.35f); }
static void sound_score(void) { play_tone(660, 990, 120, 0.20f); } // new high score entry

// ---------------------------------------------------------------------------
// Game state
// ---------------------------------------------------------------------------
static struct {
    Cell     body[SNAKE_MAX];
    int      head, len;
    SnakeDir dir, next_dir;
    Cell     food;
    int      score;
    bool     started;
    bool     game_over;
    Uint32   last_tick;
    int      grid_w, grid_h;
    int      off_x, off_y;

    bool     quit_confirm;   // B pressed during active play — waiting for A/B

    // name entry
    bool     entering_name;
    char     entry[4];     // being typed
    int      entry_pos;    // 0-2
    int      entry_ci[3];  // char index per slot (0..SNAKE_CHAR_COUNT-1)
    int      hs_rank;      // where this score will land

    // post-game-over flash timer before name entry / splash
    Uint32   gameover_at;
} sn;

static void place_food(void) {
    for (int attempt = 0; attempt < 2000; attempt++) {
        int fx = rand() % sn.grid_w, fy = rand() % sn.grid_h;
        bool hit = false;
        for (int i = 0; i < sn.len && !hit; i++) {
            int idx = (sn.head - i + SNAKE_MAX) % SNAKE_MAX;
            if (sn.body[idx].x == fx && sn.body[idx].y == fy) hit = true;
        }
        if (!hit) { sn.food.x = fx; sn.food.y = fy; return; }
    }
}

static void reset_game(void) {
    int avail_h = cfg.screen_h - SNAKE_HDR_H;
    sn.grid_w = cfg.screen_w / SNAKE_CELL;
    sn.grid_h = avail_h / SNAKE_CELL;
    sn.off_x  = (cfg.screen_w - sn.grid_w * SNAKE_CELL) / 2;
    sn.off_y  = SNAKE_HDR_H + (avail_h - sn.grid_h * SNAKE_CELL) / 2;

    sn.head = 2; sn.len = 3;
    int mx = sn.grid_w / 2, my = sn.grid_h / 2;
    sn.body[0] = (Cell){mx-2, my};
    sn.body[1] = (Cell){mx-1, my};
    sn.body[2] = (Cell){mx,   my};
    sn.dir = sn.next_dir = DIR_RIGHT;
    sn.score         = 0;
    sn.started       = false;
    sn.game_over     = false;
    sn.quit_confirm  = false;
    sn.last_tick     = 0;
    sn.entering_name = false;
    place_food();
}

// ---------------------------------------------------------------------------
void snake_enter(void) {
    srand((unsigned int)time(NULL));
    hs_load();
    audio_open();
    reset_game();
    sn.started = false;
}

static void snake_leave(void) {
    audio_close();
    current_mode = MODE_EXPLORER;
}

// ---------------------------------------------------------------------------
static int tick_ms(void) {
    int ms = 200 - (sn.score / 3) * 8;
    return ms < 65 ? 65 : ms;
}

static void step(void) {
    sn.dir = sn.next_dir;
    Cell h = sn.body[sn.head], nh = h;
    if      (sn.dir == DIR_RIGHT) nh.x++;
    else if (sn.dir == DIR_LEFT)  nh.x--;
    else if (sn.dir == DIR_UP)    nh.y--;
    else                          nh.y++;

    if (nh.x < 0 || nh.x >= sn.grid_w || nh.y < 0 || nh.y >= sn.grid_h)
        { sn.game_over = true; sound_die(); sn.gameover_at = SDL_GetTicks(); return; }

    for (int i = 0; i < sn.len - 1; i++) {
        int idx = (sn.head - i + SNAKE_MAX) % SNAKE_MAX;
        if (sn.body[idx].x == nh.x && sn.body[idx].y == nh.y)
            { sn.game_over = true; sound_die(); sn.gameover_at = SDL_GetTicks(); return; }
    }

    sn.head = (sn.head + 1) % SNAKE_MAX;
    sn.body[sn.head] = nh;

    if (nh.x == sn.food.x && nh.y == sn.food.y) {
        sn.score++;
        if (sn.len < SNAKE_MAX - 1) sn.len++;
        place_food();
        sound_eat();
    }
}

// forward declaration — defined after snake_tick
static void name_entry_button(SDL_GameControllerButton btn);

// ---------------------------------------------------------------------------
// Hold-repeat state for name entry
static Uint32 ne_held_since = 0;
static Uint32 ne_last_fire  = 0;
static int    ne_held_dir   = 0;   // +1 = up, -1 = down, 0 = none
#define NE_REPEAT_DELAY 400
#define NE_REPEAT_RATE   80

void snake_tick(Uint32 now) {
    if (!sn.started || sn.quit_confirm) return;

    // Name entry: handle hold-repeat for up/down then bail out
    if (sn.entering_name) {
        if (pad) {
            bool up   = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_UP);
            bool down = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_DOWN);
            int  dir  = up ? 1 : (down ? -1 : 0);
            if (dir != ne_held_dir) {
                ne_held_dir   = dir;
                ne_held_since = now;
                ne_last_fire  = now;
            } else if (dir != 0 && now - ne_held_since >= NE_REPEAT_DELAY
                       && now - ne_last_fire >= NE_REPEAT_RATE) {
                ne_last_fire = now;
                name_entry_button(up ? SDL_CONTROLLER_BUTTON_DPAD_UP
                                     : SDL_CONTROLLER_BUTTON_DPAD_DOWN);
            }
        }
        return;
    }
    ne_held_dir = 0;

    // 900ms after death: move to name entry or splash
    if (sn.game_over) {
        if (SDL_GetTicks() - sn.gameover_at > 900) {
            if (sn.hs_rank >= 0) {
                sn.entering_name = true;
                sn.entry_pos  = 0;
                sn.entry_ci[0] = sn.entry_ci[1] = sn.entry_ci[2] = 0;
                sn.entry[0] = sn.entry[1] = sn.entry[2] = SNAKE_CHARS[0];
                sn.entry[3] = '\0';
                sound_score();
            } else {
                reset_game();
            }
        }
        return;
    }

    if (sn.last_tick == 0) sn.last_tick = now;
    if (now - sn.last_tick >= (Uint32)tick_ms()) {
        sn.last_tick = now;
        step();
        if (sn.game_over) {
            sn.hs_rank = hs_qualifies(sn.score);
        }
    }
}

// ---------------------------------------------------------------------------
static void name_entry_button(SDL_GameControllerButton btn) {
    const char *chars = SNAKE_CHARS;
    switch (btn) {
        case SDL_CONTROLLER_BUTTON_DPAD_UP:
            sn.entry_ci[sn.entry_pos] = (sn.entry_ci[sn.entry_pos] + 1) % SNAKE_CHAR_COUNT;
            sn.entry[sn.entry_pos] = chars[sn.entry_ci[sn.entry_pos]];
            break;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
            sn.entry_ci[sn.entry_pos] = (sn.entry_ci[sn.entry_pos] - 1 + SNAKE_CHAR_COUNT) % SNAKE_CHAR_COUNT;
            sn.entry[sn.entry_pos] = chars[sn.entry_ci[sn.entry_pos]];
            break;
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:
        case SDL_CONTROLLER_BUTTON_A:
            if (sn.entry_pos < 2) {
                sn.entry_pos++;
            } else {
                // Confirm — save and go to splash showing scores
                hs_insert(sn.hs_rank, sn.entry, sn.score);
                hs_save();
                sn.entering_name = false;
                reset_game();
            }
            break;
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT:
            if (sn.entry_pos > 0) sn.entry_pos--;
            break;
        case SDL_CONTROLLER_BUTTON_B:
            // Cancel — discard score, back to splash
            sn.entering_name = false;
            reset_game();
            break;
        default: break;
    }
}

void snake_handle_button(SDL_GameControllerButton btn) {
    // Y always toggles theme
    if (btn == SDL_CONTROLLER_BUTTON_Y) {
        snake_green_mode = !snake_green_mode;
        return;
    }

    // Quit confirmation dialog (only during active play)
    if (sn.quit_confirm) {
        if (btn == SDL_CONTROLLER_BUTTON_A) {
            snake_leave();
        } else {
            sn.quit_confirm = false;   // any other button cancels
        }
        return;
    }

    if (sn.entering_name) { name_entry_button(btn); return; }

    if (sn.game_over) return;   // swallow all buttons during death pause; tick handles transition

    if (!sn.started) {
        if (btn == SDL_CONTROLLER_BUTTON_A || btn == SDL_CONTROLLER_BUTTON_START) {
            reset_game();
            sn.started   = true;
            sn.last_tick = SDL_GetTicks();
        } else if (btn == SDL_CONTROLLER_BUTTON_B) {
            snake_leave();
        }
        return;
    }

    switch (btn) {
        case SDL_CONTROLLER_BUTTON_DPAD_UP:    if (sn.dir != DIR_DOWN)  sn.next_dir = DIR_UP;    break;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN:  if (sn.dir != DIR_UP)    sn.next_dir = DIR_DOWN;  break;
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT:  if (sn.dir != DIR_RIGHT) sn.next_dir = DIR_LEFT;  break;
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: if (sn.dir != DIR_LEFT)  sn.next_dir = DIR_RIGHT; break;
        case SDL_CONTROLLER_BUTTON_B: sn.quit_confirm = true; break;
        default: break;
    }
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------
static void draw_centered(TTF_Font *f, const char *txt, int y, SDL_Color c) {
    if (!f || !txt) return;
    int w = 0; TTF_SizeText(f, txt, &w, NULL);
    draw_txt(f, txt, (cfg.screen_w - w) / 2, y, c);
}

static void draw_grid_and_food(SnakeTheme *th) {
    // Grid lines
    SDL_SetRenderDrawColor(renderer, th->grid.r, th->grid.g, th->grid.b, 255);
    for (int gx = 0; gx <= sn.grid_w; gx++)
        SDL_RenderDrawLine(renderer,
            sn.off_x + gx*SNAKE_CELL, sn.off_y,
            sn.off_x + gx*SNAKE_CELL, sn.off_y + sn.grid_h*SNAKE_CELL);
    for (int gy = 0; gy <= sn.grid_h; gy++)
        SDL_RenderDrawLine(renderer,
            sn.off_x, sn.off_y + gy*SNAKE_CELL,
            sn.off_x + sn.grid_w*SNAKE_CELL, sn.off_y + gy*SNAKE_CELL);

    // Food
    SDL_Rect fr = {sn.off_x + sn.food.x*SNAKE_CELL+3,
                   sn.off_y + sn.food.y*SNAKE_CELL+3,
                   SNAKE_CELL-6, SNAKE_CELL-6};
    SDL_SetRenderDrawColor(renderer, th->food.r, th->food.g, th->food.b, 255);
    SDL_RenderFillRect(renderer, &fr);
    SDL_SetRenderDrawColor(renderer, th->food_pip.r, th->food_pip.g, th->food_pip.b, 255);
    SDL_Rect fp = {fr.x+2, fr.y+2, 3, 3};
    SDL_RenderFillRect(renderer, &fp);
}

static void draw_snake(SnakeTheme *th) {
    for (int i = sn.len-1; i >= 0; i--) {
        int idx = (sn.head - i + SNAKE_MAX) % SNAKE_MAX;
        Cell c  = sn.body[idx];
        int pp  = (i == 0) ? 1 : 2;
        SDL_Rect sr = {sn.off_x + c.x*SNAKE_CELL+pp,
                       sn.off_y + c.y*SNAKE_CELL+pp,
                       SNAKE_CELL-pp*2, SNAKE_CELL-pp*2};
        if (i == 0) {
            SDL_SetRenderDrawColor(renderer,
                th->snake_head.r, th->snake_head.g, th->snake_head.b, 255);
        } else {
            // Lerp body colour from hi (near head) to lo (near tail)
            float t = (sn.len > 1) ? (float)(i-1)/(sn.len-1) : 0.0f;
            Uint8 r = (Uint8)(th->snake_body_hi.r + t*(th->snake_body_lo.r - th->snake_body_hi.r));
            Uint8 g = (Uint8)(th->snake_body_hi.g + t*(th->snake_body_lo.g - th->snake_body_hi.g));
            Uint8 b = (Uint8)(th->snake_body_hi.b + t*(th->snake_body_lo.b - th->snake_body_hi.b));
            SDL_SetRenderDrawColor(renderer, r, g, b, 255);
        }
        SDL_RenderFillRect(renderer, &sr);
    }
}

static void draw_high_scores(SnakeTheme *th, int y_start) {
    if (hs_count == 0) {
        draw_centered(font_footer, "No scores yet", y_start, th->text_dim);
        return;
    }
    int lh = font_footer ? TTF_FontHeight(font_footer) + 4 : 20;
    draw_centered(font_footer, "HIGH SCORES", y_start, th->text_mid);
    y_start += lh + 4;
    for (int i = 0; i < hs_count; i++) {
        char line[32];
        snprintf(line, sizeof(line), "%d. %s  %d", i+1, high_scores[i].name, high_scores[i].score);
        SDL_Color col = (i == 0) ? th->text_hi : th->text_dim;
        draw_centered(font_footer, line, y_start + i*lh, col);
    }
}

static void draw_name_entry(SnakeTheme *th) {
    // Dim overlay over game
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 180);
    SDL_Rect ov = {0, 0, cfg.screen_w, cfg.screen_h};
    SDL_RenderFillRect(renderer, &ov);
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);

    int cy = cfg.screen_h / 2 - 50;
    char rank_msg[32];
    snprintf(rank_msg, sizeof(rank_msg), "Rank #%d  Score: %d", sn.hs_rank+1, sn.score);
    draw_centered(font_header, "NEW HIGH SCORE!", cy, th->header_text);
    draw_centered(font_footer, rank_msg, cy + cfg.font_size_header + 6, th->header_text);

    // 3 character slots — extra footer height ensures ^ arrow clears rank row
    int slot_w = cfg.font_size_header + 16;
    int slot_gap = 8;
    int total_w = slot_w*3 + slot_gap*2;
    int sx = (cfg.screen_w - total_w) / 2;
    int sy = cy + cfg.font_size_header + cfg.font_size_footer * 2 + 20;

    for (int i = 0; i < 3; i++) {
        int bx = sx + i*(slot_w + slot_gap);
        bool active = (i == sn.entry_pos);
        SDL_Rect box = {bx, sy, slot_w, slot_w};
        // Box border — bright on active slot
        SDL_Color box_col = active ? th->header_text : th->text_dim;
        SDL_SetRenderDrawColor(renderer, box_col.r, box_col.g, box_col.b, 255);
        SDL_RenderDrawRect(renderer, &box);
        // Character
        char ch[2] = {sn.entry[i], '\0'};
        int cw = 0;
        if (font_header) TTF_SizeText(font_header, ch, &cw, NULL);
        draw_txt(font_header, ch,
                 bx + (slot_w - cw)/2, sy + (slot_w - cfg.font_size_header)/2,
                 active ? th->header_text : th->text_dim);
        // Arrow indicators on active slot
        if (active) {
            draw_txt(font_footer, "^", bx + slot_w/2 - 4, sy - cfg.font_size_footer - 2, th->header_text);
            draw_txt(font_footer, "v", bx + slot_w/2 - 4, sy + slot_w + 2,               th->header_text);
        }
    }

    int hint_y = sy + slot_w + cfg.font_size_footer + 14;
    draw_centered(font_footer, "Up/Dn: Change   Right/A: Next   B: Cancel", hint_y, th->header_text);
}

void snake_draw(void) {
    SnakeTheme th = current_theme();

    // Background + header
    SDL_SetRenderDrawColor(renderer, th.bg.r, th.bg.g, th.bg.b, 255);
    SDL_RenderClear(renderer);
    SDL_SetRenderDrawColor(renderer, th.header_bg.r, th.header_bg.g, th.header_bg.b, 255);
    SDL_Rect hdr = {0, 0, cfg.screen_w, SNAKE_HDR_H};
    SDL_RenderFillRect(renderer, &hdr);

    // Header text
    char score_buf[24];
    snprintf(score_buf, sizeof(score_buf), "Score: %d", sn.score);
    draw_txt(font_header, "SNAKE",    14, (SNAKE_HDR_H - cfg.font_size_header)/2, th.header_text);
    draw_txt(font_menu,   score_buf,
             cfg.screen_w/2 - 40, (SNAKE_HDR_H - cfg.font_size_menu)/2, th.text_mid);
    {
        const char *hint = "B:Exit  Y:Theme";
        int hw = 0;
        if (font_footer) TTF_SizeText(font_footer, hint, &hw, NULL);
        draw_txt(font_footer, hint,
                 cfg.screen_w - hw - 8, (SNAKE_HDR_H - cfg.font_size_footer)/2, th.header_text);
    }

    draw_grid_and_food(&th);

    // Splash screen
    if (!sn.started) {
        int cy = cfg.screen_h / 2 - 30;
        draw_centered(font_header, "Press A to start", cy, th.text_hi);
        draw_centered(font_footer, "D-pad: Steer   B: Exit   Y: Theme", cy + cfg.font_size_header + 10, th.text_dim);
        int hs_y = cy + cfg.font_size_header + cfg.font_size_footer + 30;
        draw_high_scores(&th, hs_y);
        return;
    }

    draw_snake(&th);

    // Game-over overlay (shown for 900ms before transitioning)
    if (sn.game_over && !sn.entering_name) {
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 160);
        SDL_Rect ov = {0, 0, cfg.screen_w, cfg.screen_h};
        SDL_RenderFillRect(renderer, &ov);
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);

        char msg[40];
        snprintf(msg, sizeof(msg), "GAME OVER   Score: %d", sn.score);
        draw_centered(font_header, msg,    cfg.screen_h/2 - 20, th.header_text);
        draw_centered(font_footer, "...",  cfg.screen_h/2 + 12, th.header_text);
    }

    if (sn.entering_name) draw_name_entry(&th);

    // Quit confirm overlay
    if (sn.quit_confirm) {
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 180);
        SDL_Rect ov = {0, 0, cfg.screen_w, cfg.screen_h};
        SDL_RenderFillRect(renderer, &ov);
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
        draw_centered(font_header, "Quit?",           cfg.screen_h/2 - cfg.font_size_header - 4, th.header_text);
        draw_centered(font_footer, "A: Yes   B: No",  cfg.screen_h/2 + 4,                        th.header_text);
    }
}
