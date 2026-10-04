#ifndef POLL_H
#define POLL_H

typedef void (*poll_fn_t)(void *context);
int poll_register(poll_fn_t fn, void *context);
void poll_run(void);

#endif
