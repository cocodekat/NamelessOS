#ifndef FB_H
#define FB_H

// Sets up the framebuffer text console using whatever Limine handed us.
// Safe to call even if no framebuffer is available — everything else in
// this module just becomes a no-op in that case.
void fb_console_init(void);

// Feeds one byte through the console. Understands plain characters plus
// the small set of ANSI escapes this codebase already emits: "\x1b[2J"
// (clear), "\x1b[H" (home), and "\x1b[<row>;<col>H" (position cursor).
void fb_console_putc(char c);

#endif