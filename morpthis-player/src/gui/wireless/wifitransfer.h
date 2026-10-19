#ifndef GUI_WIFITRANSFER_H
#define GUI_WIFITRANSFER_H

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

// "Transfer": the page that puts the microSD on the network so a browser on
// the same Wi-Fi can drop music onto it.
//
// The page itself is the switch, exactly as in the stock player: the server
// comes up when the page opens and goes down when it is left. Hence the
// warning the firmware prints on it -- "keep this page open; closing it closes
// the connection" -- which is literal, not advice.

extern lv_obj_t *wifitransfer_screen;

void wifitransfer_page_init(gui_config_t *cfg);

// Opens the page with the server starting, as if its switch had been turned on
// there: the control centre's button. The caller has checked that the server
// is available and the Wi-Fi connected.
void wifitransfer_page_open_started(void);

// Stops the server and turns the page's switch off, wherever the page is.
void wifitransfer_page_stop(void);

#endif /* GUI_WIFITRANSFER_H */
