#include "sdk_internal.h"

void morpthis_input_set_button_cb(morpthis_button_cb cb, void *user) {
	if (!sdk_app) {
		return;
	}
	sdk_app->button_cb = cb;
	sdk_app->button_user = user;
}

void morpthis_input_set_touch_cb(morpthis_touch_cb cb, void *user) {
	if (!sdk_app) {
		return;
	}
	sdk_app->touch_cb = cb;
	sdk_app->touch_user = user;
}
