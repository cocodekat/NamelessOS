#ifndef DRIVERS_WIFI_IWLWIFI_H
#define DRIVERS_WIFI_IWLWIFI_H

int iwlwifi_init(void);
int iwlwifi_prepare_transport(void);
int iwlwifi_prepare_queues(void);
void iwlwifi_print_diagnostics(void);

#endif
