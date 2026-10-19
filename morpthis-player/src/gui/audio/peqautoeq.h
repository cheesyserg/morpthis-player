#ifndef PEQAUTOEQ_H
#define PEQAUTOEQ_H

#include <stdbool.h>

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

void peqautoeq_init(gui_config_t *cfg);
lv_obj_t *peqautoeq_screen(void);
void peqautoeq_open(void);
void peqautoeq_set_reload_cb(void (*cb)(void));

// Whether AutoEq can reach the network: Wi-Fi connected with an address.
bool peqautoeq_network_ready(void);

// False when it can. Otherwise says that AutoEq needs Wi-Fi and returns true.
bool peqautoeq_network_needed(void);

#endif /* PEQAUTOEQ_H */
