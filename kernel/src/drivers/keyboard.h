// keyboard.h
#ifndef KEYBOARD_H
#define KEYBOARD_H

// Blocks until a key is available and returns its translated byte.
// Deliberately matches what serial_getc()/serial_readline() already
// produce: printable ASCII, '\n'/'\b', Ctrl+<letter> as 0x01-0x1A, and
// arrow keys as the 3-byte escape sequence ESC '[' 'A'/'B'/'C'/'D' — so
// your editor's input-handling code works unchanged regardless of
// whether input came from serial or a real keyboard.
char kb_getc(void);

#endif