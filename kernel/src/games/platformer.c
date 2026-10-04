// A compact, freestanding platformer template.
// Controls: A/D (or J/L) move, Space/W jump, R restart, Q/Esc quit.
// Start by editing the add_platform() calls in reset_game().

#include <stdint.h>
#include <stddef.h>
#include <limine.h>
#include "drivers/xhci.h"
#include "poll.h"

extern volatile struct limine_framebuffer_request framebuffer_request;

#define GAME_W 480
#define GAME_H 320
#define PLAYER_W 18
#define PLAYER_H 26
#define PLATFORM_MAX 12
#define FP_SHIFT 8
#define GRAVITY 80
#define MOVE_SPEED 800
#define JUMP_SPEED (-1600)
#define GROUND_Y 292
#define PS2_DATA_PORT 0x60
#define PS2_STATUS_PORT 0x64
#ifndef GAME_DELAY_ITERS
#define GAME_DELAY_ITERS 90000
#endif

typedef struct
{
    int x, y, w, h;
} platform_t;
typedef struct
{
    struct limine_framebuffer *fb;
    uint8_t *base;
    int screen_w, screen_h, game_x, game_y, scale;
    int player_x_fp, player_y_fp, player_vx_fp, player_vy_fp;
    int grounded, won, dead;
    int ps2_left_held, ps2_right_held;
    int usb_jump_held;
    int jump_buffer_frames;
    int coyote_frames;
    platform_t platforms[PLATFORM_MAX];
    unsigned platform_count;
} platformer_state_t;

static uint32_t backbuffer[GAME_W * GAME_H];

static inline uint8_t inb(uint16_t port)
{
    uint8_t v;
    __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

static void game_delay(void)
{
    for (volatile unsigned i = 0; i < GAME_DELAY_ITERS; ++i)
        __asm__ volatile("pause");
}

static uint32_t rgb(platformer_state_t *s, uint8_t r, uint8_t g, uint8_t b)
{
    uint32_t rr = r, gg = g, bb = b;
    if (s->fb->red_mask_size < 8)
        rr >>= 8 - s->fb->red_mask_size;
    if (s->fb->green_mask_size < 8)
        gg >>= 8 - s->fb->green_mask_size;
    if (s->fb->blue_mask_size < 8)
        bb >>= 8 - s->fb->blue_mask_size;
    return (rr << s->fb->red_mask_shift) |
           (gg << s->fb->green_mask_shift) |
           (bb << s->fb->blue_mask_shift);
}

static void clear_backbuffer(uint32_t color)
{
    for (size_t i = 0; i < (size_t)GAME_W * GAME_H; ++i)
        backbuffer[i] = color;
}

static void rect(int x, int y, int w, int h, uint32_t color)
{
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
        for (int xx = x; xx < x + w; ++xx)
            backbuffer[yy * GAME_W + xx] = color;
}

static void put_pixel(platformer_state_t *s, int x, int y, uint32_t color)
{
    if ((unsigned)x >= (unsigned)s->screen_w ||
        (unsigned)y >= (unsigned)s->screen_h)
        return;
    uint8_t *p = s->base + (size_t)y * s->fb->pitch +
                 (size_t)x * (s->fb->bpp / 8);
    if (s->fb->bpp == 32)
        *(uint32_t *)p = color;
    else if (s->fb->bpp == 24)
    {
        p[0] = (uint8_t)color;
        p[1] = (uint8_t)(color >> 8);
        p[2] = (uint8_t)(color >> 16);
    }
    else if (s->fb->bpp == 16)
        *(uint16_t *)p = (uint16_t)color;
}

static void present(platformer_state_t *s)
{
    for (int y = 0; y < GAME_H; ++y)
        for (int x = 0; x < GAME_W; ++x)
            for (int sy = 0; sy < s->scale; ++sy)
                for (int sx = 0; sx < s->scale; ++sx)
                    put_pixel(s, s->game_x + x * s->scale + sx,
                              s->game_y + y * s->scale + sy,
                              backbuffer[y * GAME_W + x]);
}

enum
{
    KEY_NONE,
    KEY_LEFT,
    KEY_RIGHT,
    KEY_JUMP,
    KEY_RESTART,
    KEY_QUIT
};

static int poll_input(platformer_state_t *s)
{
    poll_run();
    int c = xhci_poll_key();
    if (c >= 0)
    {
        if (c == 'r' || c == 'R')
            return KEY_RESTART;
        if (c == 'q' || c == 'Q' || c == 27)
            return KEY_QUIT;
    }

    // Read jump directly from the current USB HID report. The translated
    // character queue is meant for typing and can lose game-like timing.
    int usb_jump_down = xhci_key_down(0x2c) || // Space
                        xhci_key_down(0x1a) || // W
                        xhci_key_down(0x28);   // Enter
    int usb_jump_pressed = usb_jump_down && !s->usb_jump_held;
    s->usb_jump_held = usb_jump_down;
    if (usb_jump_pressed)
        return KEY_JUMP;

    if (!(inb(PS2_STATUS_PORT) & 1))
        return KEY_NONE;
    uint8_t sc = inb(PS2_DATA_PORT);
    int pressed = (sc & 0x80) == 0;
    uint8_t code = sc & 0x7f;
    switch (code)
    {
    case 0x1e:
    case 0x24:
        s->ps2_left_held = pressed; // A, J
        return KEY_NONE;
    case 0x20:
    case 0x26:
        s->ps2_right_held = pressed; // D, L
        return KEY_NONE;
    case 0x39:
    case 0x11:
        return pressed ? KEY_JUMP : KEY_NONE; // Space, W
    case 0x13:
        return pressed ? KEY_RESTART : KEY_NONE;
    case 0x10:
    case 0x01:
        return pressed ? KEY_QUIT : KEY_NONE;
    default:
        return KEY_NONE;
    }
}

static void add_platform(platformer_state_t *s, int x, int y, int w, int h)
{
    if (s->platform_count < PLATFORM_MAX)
        s->platforms[s->platform_count++] = (platform_t){x, y, w, h};
}

static void reset_game(platformer_state_t *s)
{
    s->player_x_fp = 36 << FP_SHIFT;
    s->player_y_fp = (GROUND_Y - PLAYER_H) << FP_SHIFT;
    s->player_vx_fp = s->player_vy_fp = 0;
    s->grounded = 1;
    s->won = s->dead = 0;
    s->ps2_left_held = s->ps2_right_held = 0;
    s->usb_jump_held = 0;
    s->jump_buffer_frames = 0;
    s->coyote_frames = 0;
    s->platform_count = 0;

    // LEVEL TEMPLATE: x, y, width, height in game pixels.
    add_platform(s, 0, GROUND_Y, GAME_W, GAME_H - GROUND_Y);
    add_platform(s, 85, 238, 76, 12);
    add_platform(s, 210, 202, 72, 12);
    add_platform(s, 335, 158, 82, 12);
    add_platform(s, 230, 112, 66, 12);
    add_platform(s, 365, 74, 70, 12); // Reach this flag to win.
}

static int overlaps(int ax, int ay, int aw, int ah, const platform_t *b)
{
    return ax < b->x + b->w && ax + aw > b->x &&
           ay < b->y + b->h && ay + ah > b->y;
}

static void update_game(platformer_state_t *s, int key)
{
    // USB HID usages: A=0x04, D=0x07, J=0x0d, L=0x0f.
    int left_held = s->ps2_left_held ||
                    xhci_key_down(0x04) || xhci_key_down(0x0d);
    int right_held = s->ps2_right_held ||
                     xhci_key_down(0x07) || xhci_key_down(0x0f);

    if (left_held && !right_held)
        s->player_vx_fp = -MOVE_SPEED;
    else if (right_held && !left_held)
        s->player_vx_fp = MOVE_SPEED;
    else
        s->player_vx_fp = 0;
    // Keep a jump press briefly instead of requiring it on the exact landing
    // frame. Coyote time also permits a jump just after leaving an edge.
    if (key == KEY_JUMP)
        s->jump_buffer_frames = 7;
    else if (s->jump_buffer_frames > 0)
        --s->jump_buffer_frames;

    if (s->grounded)
        s->coyote_frames = 5;
    else if (s->coyote_frames > 0)
        --s->coyote_frames;

    if (s->jump_buffer_frames > 0 && s->coyote_frames > 0)
    {
        s->player_vy_fp = JUMP_SPEED;
        s->grounded = 0;
        s->jump_buffer_frames = 0;
        s->coyote_frames = 0;
    }
    s->player_x_fp += s->player_vx_fp;
    int x = s->player_x_fp >> FP_SHIFT;
    if (x < 0)
        s->player_x_fp = 0;
    if (x > GAME_W - PLAYER_W)
        s->player_x_fp = (GAME_W - PLAYER_W) << FP_SHIFT;

    int old_y = s->player_y_fp >> FP_SHIFT;
    s->player_vy_fp += GRAVITY;
    if (s->player_vy_fp > 900)
        s->player_vy_fp = 900;
    s->player_y_fp += s->player_vy_fp;
    int y = s->player_y_fp >> FP_SHIFT;
    x = s->player_x_fp >> FP_SHIFT;
    s->grounded = 0;

    // Land only while falling, so the player can jump up through platforms.
    if (s->player_vy_fp >= 0)
    {
        for (unsigned i = 0; i < s->platform_count; ++i)
        {
            platform_t *p = &s->platforms[i];
            if (overlaps(x, y, PLAYER_W, PLAYER_H, p) &&
                old_y + PLAYER_H <= p->y)
            {
                s->player_y_fp = (p->y - PLAYER_H) << FP_SHIFT;
                s->player_vy_fp = 0;
                s->grounded = 1;
                y = p->y - PLAYER_H;
                break;
            }
        }
    }
    if (y > GAME_H + 40)
        s->dead = 1;
    if (x + PLAYER_W > 365 && x < 435 && y + PLAYER_H <= 74)
        s->won = 1;
}

static void render_game(platformer_state_t *s)
{
    uint32_t sky = rgb(s, 83, 180, 232);
    uint32_t grass = rgb(s, 66, 160, 76);
    uint32_t dirt = rgb(s, 125, 83, 45);
    clear_backbuffer(sky);
    for (unsigned i = 0; i < s->platform_count; ++i)
    {
        platform_t *p = &s->platforms[i];
        rect(p->x, p->y, p->w, 4, grass);
        rect(p->x, p->y + 4, p->w, p->h - 4, dirt);
    }
    uint32_t red = rgb(s, 244, 80, 94);
    rect(390, 42, 4, 32, red);
    rect(394, 42, 22, 13, red);
    rect(s->player_x_fp >> FP_SHIFT, s->player_y_fp >> FP_SHIFT,
         PLAYER_W, PLAYER_H, rgb(s, 245, 198, 54));
    if (s->dead)
        rect(0, 0, GAME_W, 8, rgb(s, 190, 45, 45));
    if (s->won)
        rect(0, 0, GAME_W, 8, rgb(s, 245, 220, 50));
    present(s);
}

static int init_state(platformer_state_t *s)
{
    if (!framebuffer_request.response ||
        framebuffer_request.response->framebuffer_count < 1)
        return -1;
    s->fb = framebuffer_request.response->framebuffers[0];
    if (!s->fb || !s->fb->address ||
        (s->fb->bpp != 16 && s->fb->bpp != 24 && s->fb->bpp != 32))
        return -1;
    s->base = (uint8_t *)s->fb->address;
    s->screen_w = (int)s->fb->width;
    s->screen_h = (int)s->fb->height;
    int sx = s->screen_w / GAME_W, sy = s->screen_h / GAME_H;
    s->scale = sx < sy ? sx : sy;
    if (s->scale < 1)
        s->scale = 1;
    s->game_x = (s->screen_w - GAME_W * s->scale) / 2;
    s->game_y = (s->screen_h - GAME_H * s->scale) / 2;
    if (s->game_x < 0)
        s->game_x = 0;
    if (s->game_y < 0)
        s->game_y = 0;
    reset_game(s);
    return 0;
}

int platformer_run(void)
{
    platformer_state_t s = {0};
    if (init_state(&s) != 0)
        return -1;
    for (;;)
    {
        int key = poll_input(&s);
        if (key == KEY_QUIT)
            return 0;
        if (key == KEY_RESTART)
            reset_game(&s);
        else if (!s.dead && !s.won)
            update_game(&s, key);
        render_game(&s);
        game_delay();
    }
}
