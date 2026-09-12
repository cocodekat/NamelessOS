#include <stdint.h>
#include <limine.h>
#include <memory.h>
#include "fb.h"
#include "font8x16.h"

extern volatile struct limine_framebuffer_request framebuffer_request;

#define GLYPH_W 8
#define GLYPH_H 16

#define FB_MAX_COLS 320
#define FB_MAX_ROWS 200

#define COLOR_BG 0x00101014u
#define COLOR_FG 0x00E6E6E6u

static uint32_t *fb_addr = NULL;
static size_t fb_width = 0, fb_height = 0, fb_pitch_pixels = 0;
static int fb_available = 0;

static int cols = 0, rows = 0;
static int cur_row = 0, cur_col = 0;
static int indicator_row = -1, indicator_col = -1;

static char screen_chars[FB_MAX_ROWS][FB_MAX_COLS];

static enum { ST_NORMAL, ST_ESC, ST_CSI } state = ST_NORMAL;
static int csi_params[2];
static int csi_param_count;

static void draw_glyph(int row, int col, char c, int inverted) {
    if (!fb_available) return;
    if (row < 0 || row >= rows || col < 0 || col >= cols) return;

    const unsigned char *glyph = (c >= 0 && c < 128) ? font8x16[(int)c] : font8x16[0];
    uint32_t fg = inverted ? COLOR_BG : COLOR_FG;
    uint32_t bg = inverted ? COLOR_FG : COLOR_BG;

    size_t px0 = (size_t)col * GLYPH_W;
    size_t py0 = (size_t)row * GLYPH_H;

    for (int y = 0; y < GLYPH_H; y++) {
        unsigned char bits = glyph[y];
        uint32_t *rowptr = fb_addr + (py0 + y) * fb_pitch_pixels + px0;
        for (int x = 0; x < GLYPH_W; x++) {
            int set = (bits >> (7 - x)) & 1;
            rowptr[x] = set ? fg : bg;
        }
    }
}

static void fb_clear_screen(void) {
    if (!fb_available) return;

    for (size_t y = 0; y < fb_height; y++) {
        uint32_t *rowptr = fb_addr + y * fb_pitch_pixels;
        for (size_t x = 0; x < fb_width; x++) rowptr[x] = COLOR_BG;
    }
    for (int r = 0; r < rows; r++)
        for (int c = 0; c < cols; c++)
            screen_chars[r][c] = ' ';

    cur_row = 0;
    cur_col = 0;
    indicator_row = -1;
    indicator_col = -1;
}

static void fb_scroll(void) {
    if (!fb_available) return;

    size_t row_bytes = fb_pitch_pixels * sizeof(uint32_t);
    memmove(fb_addr, fb_addr + GLYPH_H * fb_pitch_pixels, row_bytes * (fb_height - GLYPH_H));

    for (size_t y = fb_height - GLYPH_H; y < fb_height; y++) {
        uint32_t *rowptr = fb_addr + y * fb_pitch_pixels;
        for (size_t x = 0; x < fb_width; x++) rowptr[x] = COLOR_BG;
    }

    for (int r = 0; r < rows - 1; r++)
        for (int c = 0; c < cols; c++)
            screen_chars[r][c] = screen_chars[r + 1][c];
    for (int c = 0; c < cols; c++) screen_chars[rows - 1][c] = ' ';
}

// Moves the highlighted cursor cell, restoring whatever was actually drawn
// at the previous cell so we don't leave a stray highlighted block behind
// when the cursor moves without new text being typed (e.g. arrow keys).
static void set_indicator(int row, int col) {
    if (indicator_row >= 0) {
        draw_glyph(indicator_row, indicator_col, screen_chars[indicator_row][indicator_col], 0);
    }
    if (row >= 0 && row < rows && col >= 0 && col < cols) {
        draw_glyph(row, col, screen_chars[row][col], 1);
        indicator_row = row;
        indicator_col = col;
    } else {
        indicator_row = -1;
        indicator_col = -1;
    }
}

static void handle_csi(char letter) {
    if (letter == 'J') {
        fb_clear_screen();
    } else if (letter == 'H') {
        int row = (csi_param_count >= 1 && csi_params[0] > 0) ? csi_params[0] : 1;
        int col = (csi_param_count >= 2 && csi_params[1] > 0) ? csi_params[1] : 1;
        cur_row = row - 1;
        cur_col = col - 1;
        if (cur_row < 0) cur_row = 0;
        if (cur_col < 0) cur_col = 0;
        set_indicator(cur_row, cur_col);
    }
}

static void put_plain_char(char c) {
    if (c == '\n') {
        cur_col = 0;
        cur_row++;
        if (cur_row >= rows) { fb_scroll(); cur_row = rows - 1; }
        return;
    }
    if (c == '\r') {
        cur_col = 0;
        return;
    }
    if (c == '\b') {
        if (cur_col > 0) cur_col--;
        return;
    }

    if (cur_row < rows && cur_col < cols) {
        screen_chars[cur_row][cur_col] = c;
        draw_glyph(cur_row, cur_col, c, 0);
    }

    cur_col++;
    if (cur_col >= cols) {
        cur_col = 0;
        cur_row++;
        if (cur_row >= rows) { fb_scroll(); cur_row = rows - 1; }
    }
}

void fb_console_init(void) {
    if (framebuffer_request.response == NULL || framebuffer_request.response->framebuffer_count < 1) {
        fb_available = 0;
        return;
    }

    struct limine_framebuffer *fb = framebuffer_request.response->framebuffers[0];

    if (fb->bpp != 32) {
        // This console assumes 32 bits per pixel, which is what Limine
        // hands back in practice on QEMU/UEFI. Bail out cleanly if that
        // ever isn't true rather than mis-render.
        fb_available = 0;
        return;
    }

    fb_addr = (uint32_t *)fb->address;
    fb_width = fb->width;
    fb_height = fb->height;
    fb_pitch_pixels = fb->pitch / sizeof(uint32_t);

    cols = (int)(fb_width / GLYPH_W);
    rows = (int)(fb_height / GLYPH_H);
    if (cols > FB_MAX_COLS) cols = FB_MAX_COLS;
    if (rows > FB_MAX_ROWS) rows = FB_MAX_ROWS;

    fb_available = 1;
    state = ST_NORMAL;
    fb_clear_screen();
}

void fb_console_putc(char c) {
    if (!fb_available) return;

    if (state == ST_NORMAL) {
        if (c == 0x1B) { state = ST_ESC; return; }
        put_plain_char(c);
        return;
    }

    if (state == ST_ESC) {
        if (c == '[') {
            state = ST_CSI;
            csi_param_count = 0;
            csi_params[0] = 0;
            csi_params[1] = 0;
        } else {
            state = ST_NORMAL;
        }
        return;
    }

    // state == ST_CSI
    if (c >= '0' && c <= '9') {
        if (csi_param_count == 0) csi_param_count = 1;
        int idx = (csi_param_count > 2) ? 1 : csi_param_count - 1;
        csi_params[idx] = csi_params[idx] * 10 + (c - '0');
        return;
    }
    if (c == ';') {
        if (csi_param_count < 2) csi_param_count++;
        return;
    }

    handle_csi(c);
    state = ST_NORMAL;
}