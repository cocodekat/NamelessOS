#include "poll.h"

#define POLL_MAX_HANDLERS 8

typedef struct { poll_fn_t fn; void *context; } poll_handler_t;
static poll_handler_t handlers[POLL_MAX_HANDLERS];
static int handler_count;

int poll_register(poll_fn_t fn, void *context)
{
    if (fn == 0 || handler_count == POLL_MAX_HANDLERS)
        return -1;
    handlers[handler_count++] = (poll_handler_t){fn, context};
    return 0;
}

void poll_run(void)
{
    for (int i = 0; i < handler_count; i++)
        handlers[i].fn(handlers[i].context);
}
