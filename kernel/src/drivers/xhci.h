#ifndef XHCI_H
#define XHCI_H

// Finds the xHCI controller over PCI, brings it up, and if a keyboard is
// plugged into any root port, enumerates it as a HID boot-protocol
// keyboard. Prints progress to serial as it goes -- if bring-up fails on
// real hardware, the serial log is what tells you where. Safe to call
// even if there's no xHCI controller at all (just logs and returns).
void xhci_init(void);

// Non-blocking. Returns the next translated key byte using the same
// convention as keyboard.c's kb_getc() (printable ASCII, '\n'/'\b'/'\t',
// Ctrl+letter as 0x01-0x1A, arrow keys as ESC '[' 'A'/'B'/'C'/'D'), or -1
// if nothing is available right now. Always returns -1 if xhci_init()
// didn't find a working keyboard.
int xhci_poll_key(void);

#endif