#ifndef NET_DHCP_H
#define NET_DHCP_H
void dhcp_init(void);
/* Reset the client and immediately enqueue a new DHCP DISCOVER.
   Returns 0 when queued, or a negative netdev error. */
int dhcp_restart(void);
const char *dhcp_state_name(void);
#endif
