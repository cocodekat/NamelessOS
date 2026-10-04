// flappy_bird.c
//
// Tiny framebuffer Flappy Bird for a freestanding x86_64 Limine kernel.
//
// Controls:
//   SPACE / ENTER  flap
//   R              restart after game over
//   Q / ESC        quit back to shell
//
// Rendering:
//   The whole game frame is drawn into an off-screen 480x640 backbuffer.
//   Only after the frame is complete is it copied to Limine's framebuffer.
//   This prevents the visible "blank/gray frame" flicker caused by drawing
//   directly to the displayed framebuffer.
//
// No libc, malloc, floating point, textures, or GPU driver required.

#include <stdint.h>
#include <stddef.h>
#include <limine.h>

#include "drivers/xhci.h"
#include "poll.h"

// Defined by main.c.
extern volatile struct limine_framebuffer_request framebuffer_request;

// --------------------------------------------------------------------------
// Tuning
// --------------------------------------------------------------------------

#define GAME_W 480
#define GAME_H 640

#define BIRD_X 115
#define BIRD_W 26
#define BIRD_H 20

#define PIPE_W 58
#define PIPE_GAP 165
#define PIPE_SPEED 2
#define PIPE_COUNT 3
#define PIPE_SPACING 210

// Fixed-point physics, 8 fractional bits.
#define FP_SHIFT 8
#define GRAVITY 45
#define FLAP_VELOCITY (-900)

#define GROUND_H 54

// Legacy PS/2 ports.
#define PS2_DATA_PORT 0x60
#define PS2_STATUS_PORT 0x64

// Temporary frame pacing.
// You said ~80000 feels reasonable on your current machine.
// Replace this later with a real timer/sleep API.
#ifndef GAME_DELAY_ITERS
#define GAME_DELAY_ITERS 120000
#endif

typedef struct
{
    int x;
    int gap_y;
    int counted;
} pipe_t;

typedef struct
{
    struct limine_framebuffer *fb;
    uint8_t *base;

    int screen_w;
    int screen_h;

    int game_x;
    int game_y;
    int scale;

    int bird_y_fp;
    int bird_vy_fp;

    pipe_t pipes[PIPE_COUNT];

    unsigned score;
    uint32_t rng;
    int dead;
    int test;
} flappy_state_t;

// --------------------------------------------------------------------------
// Backbuffer
// --------------------------------------------------------------------------
//
// 480 * 640 * 4 = 1,228,800 bytes (~1.17 MiB).
//
// This is static, so it lands in .bss and does not require malloc.

static uint32_t backbuffer[GAME_W * GAME_H];

// --------------------------------------------------------------------------
// Low-level helpers
// --------------------------------------------------------------------------

static inline uint8_t inb(uint16_t port)
{
    uint8_t v;
    __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

static inline uint64_t rdtsc(void)
{
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static void fb_game_delay(void)
{
    for (volatile unsigned i = 0; i < GAME_DELAY_ITERS; ++i)
        __asm__ volatile("pause");
}

// Pack RGB into the format Limine told us the framebuffer uses.
static uint32_t pack_rgb(struct limine_framebuffer *fb,
                         uint8_t r,
                         uint8_t g,
                         uint8_t b)
{
    uint32_t pixel = 0;

    uint32_t rr = r;
    uint32_t gg = g;
    uint32_t bb = b;

    if (fb->red_mask_size < 8)
        rr >>= (8 - fb->red_mask_size);
    else if (fb->red_mask_size > 8)
        rr <<= (fb->red_mask_size - 8);

    if (fb->green_mask_size < 8)
        gg >>= (8 - fb->green_mask_size);
    else if (fb->green_mask_size > 8)
        gg <<= (fb->green_mask_size - 8);

    if (fb->blue_mask_size < 8)
        bb >>= (8 - fb->blue_mask_size);
    else if (fb->blue_mask_size > 8)
        bb <<= (fb->blue_mask_size - 8);

    pixel |= rr << fb->red_mask_shift;
    pixel |= gg << fb->green_mask_shift;
    pixel |= bb << fb->blue_mask_shift;

    return pixel;
}

// --------------------------------------------------------------------------
// Backbuffer drawing
// --------------------------------------------------------------------------

static inline void backbuffer_put_pixel(int x, int y, uint32_t color)
{
    if ((unsigned)x >= GAME_W || (unsigned)y >= GAME_H)
        return;

    backbuffer[y * GAME_W + x] = color;
}

static void backbuffer_clear(uint32_t color)
{
    for (size_t i = 0; i < (size_t)GAME_W * GAME_H; ++i)
        backbuffer[i] = color;
}

static void game_rect(flappy_state_t *s,
                      int x,
                      int y,
                      int w,
                      int h,
                      uint32_t color)
{
    (void)s;

    if (w <= 0 || h <= 0)
        return;

    if (x < 0)
    {
        w += x;
        x = 0;
    }

    if (y < 0)
    {
        h += y;
        y = 0;
    }

    if (x + w > GAME_W)
        w = GAME_W - x;

    if (y + h > GAME_H)
        h = GAME_H - y;

    if (w <= 0 || h <= 0)
        return;

    for (int yy = y; yy < y + h; ++yy)
    {
        uint32_t *row = &backbuffer[yy * GAME_W + x];

        for (int xx = 0; xx < w; ++xx)
            row[xx] = color;
    }
}

// --------------------------------------------------------------------------
// Real framebuffer output
// --------------------------------------------------------------------------

static inline void framebuffer_put_pixel(flappy_state_t *s,
                                         int x,
                                         int y,
                                         uint32_t color)
{
    if ((unsigned)x >= (unsigned)s->screen_w ||
        (unsigned)y >= (unsigned)s->screen_h)
        return;

    uint8_t *p =
        s->base +
        (size_t)y * s->fb->pitch +
        (size_t)x * (s->fb->bpp / 8);

    switch (s->fb->bpp)
    {
    case 32:
        *(uint32_t *)p = color;
        break;

    case 24:
        p[0] = (uint8_t)(color);
        p[1] = (uint8_t)(color >> 8);
        p[2] = (uint8_t)(color >> 16);
        break;

    case 16:
        *(uint16_t *)p = (uint16_t)color;
        break;

    default:
        break;
    }
}

static void clear_real_framebuffer(flappy_state_t *s, uint32_t color)
{
    if (s->fb->bpp == 32)
    {
        for (int y = 0; y < s->screen_h; ++y)
        {
            uint32_t *row =
                (uint32_t *)(s->base + (size_t)y * s->fb->pitch);

            for (int x = 0; x < s->screen_w; ++x)
                row[x] = color;
        }

        return;
    }

    for (int y = 0; y < s->screen_h; ++y)
        for (int x = 0; x < s->screen_w; ++x)
            framebuffer_put_pixel(s, x, y, color);
}

// Copy the COMPLETE backbuffer to the visible framebuffer.
//
// For the common 32-bit, scale=1 case this copies whole rows and avoids
// per-pixel bounds checks / framebuffer address calculation.
static void present(flappy_state_t *s)
{
    // Fastest/common case.
    if (s->scale == 1 && s->fb->bpp == 32)
    {
        for (int y = 0; y < GAME_H; ++y)
        {
            int dst_y = s->game_y + y;

            if ((unsigned)dst_y >= (unsigned)s->screen_h)
                continue;

            int src_x = 0;
            int dst_x = s->game_x;
            int copy_w = GAME_W;

            if (dst_x < 0)
            {
                src_x = -dst_x;
                copy_w -= src_x;
                dst_x = 0;
            }

            if (dst_x + copy_w > s->screen_w)
                copy_w = s->screen_w - dst_x;

            if (copy_w <= 0)
                continue;

            uint32_t *dst =
                (uint32_t *)(s->base + (size_t)dst_y * s->fb->pitch) +
                dst_x;

            uint32_t *src =
                &backbuffer[y * GAME_W + src_x];

            for (int x = 0; x < copy_w; ++x)
                dst[x] = src[x];
        }

        return;
    }

    // Scaled / non-32-bit fallback.
    for (int y = 0; y < GAME_H; ++y)
    {
        for (int x = 0; x < GAME_W; ++x)
        {
            uint32_t color = backbuffer[y * GAME_W + x];

            int base_x = s->game_x + x * s->scale;
            int base_y = s->game_y + y * s->scale;

            for (int sy = 0; sy < s->scale; ++sy)
            {
                for (int sx = 0; sx < s->scale; ++sx)
                {
                    framebuffer_put_pixel(
                        s,
                        base_x + sx,
                        base_y + sy,
                        color);
                }
            }
        }
    }
}

// --------------------------------------------------------------------------
// Tiny 5x7 font
// --------------------------------------------------------------------------

typedef struct
{
    char c;
    uint8_t rows[7];
} glyph_t;

static const glyph_t glyphs[] = {
    {'0', {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E}},
    {'1', {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E}},
    {'2', {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F}},
    {'3', {0x1E, 0x01, 0x01, 0x0E, 0x01, 0x01, 0x1E}},
    {'4', {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}},
    {'5', {0x1F, 0x10, 0x10, 0x1E, 0x01, 0x01, 0x1E}},
    {'6', {0x0E, 0x10, 0x10, 0x1E, 0x11, 0x11, 0x0E}},
    {'7', {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08}},
    {'8', {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E}},
    {'9', {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x01, 0x0E}},

    {'A', {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}},
    {'C', {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E}},
    {'E', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F}},
    {'F', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10}},
    {'G', {0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0E}},
    {'I', {0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E}},
    {'L', {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F}},
    {'M', {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11}},
    {'O', {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}},
    {'P', {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10}},
    {'Q', {0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D}},
    {'R', {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11}},
    {'S', {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E}},
    {'T', {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04}},
    {'V', {0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04}},
    {'Y', {0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04}},
    {' ', {0, 0, 0, 0, 0, 0, 0}},
};

static const uint8_t *find_glyph(char c)
{
    if (c >= 'a' && c <= 'z')
        c = (char)(c - 'a' + 'A');

    for (size_t i = 0;
         i < sizeof(glyphs) / sizeof(glyphs[0]);
         ++i)
    {
        if (glyphs[i].c == c)
            return glyphs[i].rows;
    }

    return NULL;
}

static void draw_char(flappy_state_t *s,
                      int x,
                      int y,
                      char c,
                      int px,
                      uint32_t color)
{
    const uint8_t *rows = find_glyph(c);

    if (!rows)
        return;

    for (int row = 0; row < 7; ++row)
    {
        for (int col = 0; col < 5; ++col)
        {
            if (rows[row] & (1u << (4 - col)))
            {
                game_rect(
                    s,
                    x + col * px,
                    y + row * px,
                    px,
                    px,
                    color);
            }
        }
    }
}

static void draw_text(flappy_state_t *s,
                      int x,
                      int y,
                      const char *text,
                      int px,
                      uint32_t color)
{
    while (*text)
    {
        draw_char(s, x, y, *text++, px, color);
        x += 6 * px;
    }
}

static void draw_uint(flappy_state_t *s,
                      int x,
                      int y,
                      unsigned n,
                      int px,
                      uint32_t color)
{
    char buf[16];
    int len = 0;

    if (n == 0)
    {
        draw_char(s, x, y, '0', px, color);
        return;
    }

    while (n && len < (int)sizeof(buf))
    {
        buf[len++] = (char)('0' + (n % 10));
        n /= 10;
    }

    while (len > 0)
    {
        --len;
        draw_char(s, x, y, buf[len], px, color);
        x += 6 * px;
    }
}

// --------------------------------------------------------------------------
// Input
// --------------------------------------------------------------------------

enum
{
    KEY_NONE = 0,
    KEY_FLAP,
    KEY_RESTART,
    KEY_QUIT,
    KEY_TEST
};

static int poll_input(void)
{
    // Keep async kernel polling alive, including USB.
    poll_run();

    // USB/xHCI keyboard path.
    int c = xhci_poll_key();

    if (c >= 0)
    {
        if (c == ' ' || c == '\n' || c == '\r')
            return KEY_FLAP;

        if (c == 'r' || c == 'R')
            return KEY_RESTART;

        if (c == 'q' || c == 'Q' || c == 27)
            return KEY_QUIT;

        if (c == 'p' || c == 'P')
            return KEY_TEST;
    }

    // PS/2 fallback.
    if (inb(PS2_STATUS_PORT) & 1)
    {
        uint8_t sc = inb(PS2_DATA_PORT);

        // Ignore release codes.
        if (sc & 0x80)
            return KEY_NONE;

        switch (sc)
        {
        case 0x39: // Space
            return KEY_FLAP;

        case 0x1C: // Enter
            return KEY_FLAP;

        case 0x10: // Q
            return KEY_QUIT;

        case 0x13: // R
            return KEY_RESTART;

        case 0x01: // Escape
            return KEY_QUIT;

        default:
            return KEY_NONE;
        }
    }

    return KEY_NONE;
}

// --------------------------------------------------------------------------
// Game logic
// --------------------------------------------------------------------------

static uint32_t rng_next(flappy_state_t *s)
{
    uint32_t x = s->rng;

    if (!x)
        x = 0xA341316Cu;

    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;

    s->rng = x;
    return x;
}

static int random_gap_y(flappy_state_t *s)
{
    const int top_margin = 85;
    const int bottom_limit =
        GAME_H - GROUND_H - PIPE_GAP - 70;

    const int span =
        bottom_limit - top_margin;

    return top_margin +
           (int)(rng_next(s) %
                 (uint32_t)(span > 1 ? span : 1));
}

static void reset_game(flappy_state_t *s)
{
    s->bird_y_fp = (GAME_H / 2) << FP_SHIFT;
    s->bird_vy_fp = 0;
    s->score = 0;
    s->dead = 0;

    for (int i = 0; i < PIPE_COUNT; ++i)
    {
        s->pipes[i].x =
            GAME_W + 110 + i * PIPE_SPACING;

        s->pipes[i].gap_y =
            random_gap_y(s);

        s->pipes[i].counted = 0;
    }
}

static int rects_overlap(int ax,
                         int ay,
                         int aw,
                         int ah,
                         int bx,
                         int by,
                         int bw,
                         int bh)
{
    return ax < bx + bw &&
           ax + aw > bx &&
           ay < by + bh &&
           ay + ah > by;
}

static void update_game(flappy_state_t *s)
{
    s->bird_vy_fp += GRAVITY;
    s->bird_y_fp += s->bird_vy_fp;

    int bird_y = s->bird_y_fp >> FP_SHIFT;

    if (bird_y < 0)
    {
        s->bird_y_fp = 0;
        s->bird_vy_fp = 0;
        bird_y = 0;
    }

    if (bird_y + BIRD_H >= GAME_H - GROUND_H)
    {
        s->dead = 1;
        return;
    }

    for (int i = 0; i < PIPE_COUNT; ++i)
    {
        pipe_t *p = &s->pipes[i];

        p->x -= PIPE_SPEED;

        if (p->x + PIPE_W < 0)
        {
            int farthest = 0;

            for (int j = 0; j < PIPE_COUNT; ++j)
            {
                if (s->pipes[j].x > farthest)
                    farthest = s->pipes[j].x;
            }

            p->x = farthest + PIPE_SPACING;
            p->gap_y = random_gap_y(s);
            p->counted = 0;
        }

        if (!p->counted &&
            p->x + PIPE_W < BIRD_X)
        {
            p->counted = 1;
            ++s->score;
        }

        int top_h = p->gap_y;
        int bottom_y = p->gap_y + PIPE_GAP;
        int bottom_h =
            GAME_H - GROUND_H - bottom_y;

        if (rects_overlap(
                BIRD_X,
                bird_y,
                BIRD_W,
                BIRD_H,
                p->x,
                0,
                PIPE_W,
                top_h) ||
            rects_overlap(
                BIRD_X,
                bird_y,
                BIRD_W,
                BIRD_H,
                p->x,
                bottom_y,
                PIPE_W,
                bottom_h))
        {
            s->dead = 1;
            return;
        }
    }
}

// --------------------------------------------------------------------------
// Game drawing
// --------------------------------------------------------------------------

static void draw_pipe(flappy_state_t *s,
                      pipe_t *p,
                      uint32_t pipe_dark,
                      uint32_t pipe_light)
{
    int top_h = p->gap_y;
    int bottom_y = p->gap_y + PIPE_GAP;
    int bottom_h =
        GAME_H - GROUND_H - bottom_y;

    // Upper pipe.
    game_rect(
        s,
        p->x,
        0,
        PIPE_W,
        top_h,
        pipe_dark);

    game_rect(
        s,
        p->x + 6,
        0,
        PIPE_W - 12,
        top_h,
        pipe_light);

    // Lower pipe.
    game_rect(
        s,
        p->x,
        bottom_y,
        PIPE_W,
        bottom_h,
        pipe_dark);

    game_rect(
        s,
        p->x + 6,
        bottom_y,
        PIPE_W - 12,
        bottom_h,
        pipe_light);

    // Upper lip.
    game_rect(
        s,
        p->x - 5,
        top_h - 24,
        PIPE_W + 10,
        24,
        pipe_dark);

    game_rect(
        s,
        p->x + 1,
        top_h - 20,
        PIPE_W - 2,
        16,
        pipe_light);

    // Lower lip.
    game_rect(
        s,
        p->x - 5,
        bottom_y,
        PIPE_W + 10,
        24,
        pipe_dark);

    game_rect(
        s,
        p->x + 1,
        bottom_y + 4,
        PIPE_W - 2,
        16,
        pipe_light);
}

static void draw_bird(flappy_state_t *s,
                      int y,
                      uint32_t yellow,
                      uint32_t orange,
                      uint32_t white,
                      uint32_t black)
{
    // Body.
    game_rect(
        s,
        BIRD_X + 3,
        y + 3,
        18,
        14,
        yellow);

    game_rect(
        s,
        BIRD_X + 8,
        y,
        11,
        18,
        yellow);

    // Wing.
    game_rect(
        s,
        BIRD_X,
        y + 8,
        10,
        8,
        orange);

    // Eye.
    game_rect(
        s,
        BIRD_X + 19,
        y + 5,
        7,
        7,
        white);

    game_rect(
        s,
        BIRD_X + 23,
        y + 7,
        3,
        3,
        black);

    // Beak.
    game_rect(
        s,
        BIRD_X + 20,
        y + 13,
        10,
        5,
        orange);
}

static void render_game(flappy_state_t *s)
{
    uint32_t sky =
        pack_rgb(s->fb, 116, 200, 220);

    uint32_t cloud =
        pack_rgb(s->fb, 225, 245, 245);

    uint32_t grass =
        pack_rgb(s->fb, 120, 200, 70);

    uint32_t dirt =
        pack_rgb(s->fb, 225, 215, 145);

    uint32_t pipe_dark =
        pack_rgb(s->fb, 45, 130, 40);

    uint32_t pipe_light =
        pack_rgb(s->fb, 105, 205, 70);

    uint32_t yellow =
        pack_rgb(s->fb, 250, 220, 45);

    uint32_t orange =
        pack_rgb(s->fb, 245, 120, 25);

    uint32_t white =
        pack_rgb(s->fb, 255, 255, 255);

    uint32_t black =
        pack_rgb(s->fb, 30, 30, 30);

    // IMPORTANT:
    // Clear the OFF-SCREEN buffer, not the real framebuffer.
    backbuffer_clear(sky);

    // Clouds.
    game_rect(
        s,
        45,
        100,
        70,
        18,
        cloud);

    game_rect(
        s,
        62,
        88,
        38,
        18,
        cloud);

    game_rect(
        s,
        300,
        145,
        90,
        16,
        cloud);

    game_rect(
        s,
        325,
        132,
        42,
        18,
        cloud);

    // Pipes.
    for (int i = 0; i < PIPE_COUNT; ++i)
    {
        draw_pipe(
            s,
            &s->pipes[i],
            pipe_dark,
            pipe_light);
    }

    // Ground.
    game_rect(
        s,
        0,
        GAME_H - GROUND_H,
        GAME_W,
        12,
        grass);

    game_rect(
        s,
        0,
        GAME_H - GROUND_H + 12,
        GAME_W,
        GROUND_H - 12,
        dirt);

    // Bird.
    int bird_y =
        s->bird_y_fp >> FP_SHIFT;

    draw_bird(
        s,
        bird_y,
        yellow,
        orange,
        white,
        black);

    // Score.
    unsigned score = s->score;

    int digits = 1;

    for (unsigned t = score;
         t >= 10;
         t /= 10)
    {
        ++digits;
    }

    int score_px = 4;

    int score_w =
        digits * 6 * score_px -
        score_px;

    draw_uint(
        s,
        (GAME_W - score_w) / 2,
        28,
        score,
        score_px,
        white);

    // Test
    if (s->test)
    {
        draw_text(
            s,
            123,
            352,
            "Hello",
            3,
            white);
    }
    // Game over panel.
    if (s->dead)
    {
        game_rect(
            s,
            72,
            245,
            GAME_W - 144,
            145,
            black);

        draw_text(
            s,
            128,
            270,
            "GAME OVER",
            5,
            white);

        draw_text(
            s,
            123,
            325,
            "R RETRY",
            3,
            white);

        draw_text(
            s,
            123,
            352,
            "Q QUIT",
            3,
            white);
    }

    // The visible framebuffer is touched only once the whole frame is ready.
    present(s);
}

// --------------------------------------------------------------------------
// Initialization
// --------------------------------------------------------------------------

static int init_state(flappy_state_t *s)
{
    if (!framebuffer_request.response ||
        framebuffer_request.response->framebuffer_count == 0)
    {
        return -1;
    }

    s->fb =
        framebuffer_request.response->framebuffers[0];

    if (!s->fb || !s->fb->address)
        return -1;

    if (s->fb->bpp != 32 &&
        s->fb->bpp != 24 &&
        s->fb->bpp != 16)
    {
        return -1;
    }

    s->base =
        (uint8_t *)s->fb->address;

    s->screen_w =
        (int)s->fb->width;

    s->screen_h =
        (int)s->fb->height;

    int sx =
        s->screen_w / GAME_W;

    int sy =
        s->screen_h / GAME_H;

    s->scale =
        sx < sy ? sx : sy;

    if (s->scale < 1)
        s->scale = 1;

    s->game_x =
        (s->screen_w -
         GAME_W * s->scale) /
        2;

    s->game_y =
        (s->screen_h -
         GAME_H * s->scale) /
        2;

    if (s->game_x < 0)
        s->game_x = 0;

    if (s->game_y < 0)
        s->game_y = 0;

    s->rng =
        (uint32_t)(rdtsc() ^
                   (rdtsc() >> 32));

    reset_game(s);

    // Clear the real framebuffer ONCE on startup.
    // We do NOT clear it every frame anymore.
    uint32_t black =
        pack_rgb(s->fb, 30, 30, 30);

    clear_real_framebuffer(
        s,
        black);

    return 0;
}

// --------------------------------------------------------------------------
// Public entry point
// --------------------------------------------------------------------------
//
// Returns:
//   0  normal exit
//  -1  no usable Limine framebuffer

int flappy_bird_run(void)
{
    flappy_state_t s;

    // Zero state without libc.
    uint8_t *bytes =
        (uint8_t *)&s;

    for (size_t i = 0;
         i < sizeof(s);
         ++i)
    {
        bytes[i] = 0;
    }

    if (init_state(&s) != 0)
        return -1;

    render_game(&s);

    // Wait for first flap.
    for (;;)
    {
        int key =
            poll_input();

        if (key == KEY_QUIT)
            return 0;

        if (key == KEY_FLAP)
        {
            s.bird_vy_fp =
                FLAP_VELOCITY;

            break;
        }

        if (key == KEY_TEST)
        {
                }

        fb_game_delay();
    }

    // Main game loop.
    for (;;)
    {
        int key =
            poll_input();

        if (key == KEY_QUIT)
            return 0;

        if (s.dead)
        {
            if (key == KEY_RESTART ||
                key == KEY_FLAP)
            {
                reset_game(&s);

                s.bird_vy_fp =
                    FLAP_VELOCITY;
            }
        }
        else
        {
            if (key == KEY_FLAP)
            {
                s.bird_vy_fp =
                    FLAP_VELOCITY;
            }

            update_game(&s);
        }

        render_game(&s);

        fb_game_delay();
    }
}
