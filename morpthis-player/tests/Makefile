CC ?= cc
PKG_CONFIG ?= pkg-config
ROOT := ..
BUILD := .build
CPPFLAGS += -I$(ROOT)
CFLAGS ?= -O2 -g
CFLAGS += -std=gnu11 -Wall -Wextra -Werror -D_FILE_OFFSET_BITS=64 -ffunction-sections -fdata-sections
# The player is built with the Game Boy core (GB_CORE=1 in the main Makefile),
# which is what gives the More menu its seventh entry. The headless suites use
# the same flag so the page they lay out is the page the firmware builds.
CFLAGS += -DGB_CORE=1
LDLIBS += -lpthread -ldl -lm
LDFLAGS += -Wl,--gc-sections

CORE = $(ROOT)/src/system/streaming/subsonic.c \
       $(ROOT)/src/system/streaming/streambridge.c \
       $(ROOT)/src/system/streaming/subsonicstream.c \
       $(ROOT)/src/system/net/http.c $(ROOT)/src/system/net/tls.c \
       $(ROOT)/src/system/core/json.c $(ROOT)/src/system/core/md5.c \
       $(ROOT)/src/system/core/config.c $(ROOT)/src/system/playback/playlist.c

.PHONY: test sanitize clean

test: $(BUILD)/test_subsonic $(BUILD)/test_pkgmerp
	python3 run_subsonic_tests.py $(BUILD)/test_subsonic
	$(BUILD)/test_pkgmerp

$(BUILD)/test_subsonic: test_subsonic.c support.c $(CORE) $(shell find $(ROOT)/src/system -name '*.h')
	@mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ test_subsonic.c support.c $(CORE) $(LDFLAGS) $(LDLIBS)

# The package mount registry: the real pkgmerp.c, with only the loop/mount
# syscalls wrapped. It catches a free-slot lookup that also matches the mount
# point, which made the first package mount fail with "too many".
PKGMERP_SRC = test_pkgmerp.c $(ROOT)/src/system/apps/pkgmerp.c

$(BUILD)/test_pkgmerp: $(PKGMERP_SRC) $(ROOT)/src/system/apps/pkgmerp.h
	@mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $(PKGMERP_SRC) $(LDFLAGS) $(LDLIBS) \
		-Wl,--wrap=open -Wl,--wrap=open64 -Wl,--wrap=ioctl -Wl,--wrap=mount \
		-Wl,--wrap=umount -Wl,--wrap=umount2 -Wl,--wrap=mkdir

.PHONY: pkgmerp
pkgmerp: $(BUILD)/test_pkgmerp
	$(BUILD)/test_pkgmerp

sanitize:
	$(MAKE) BUILD=.build/sanitize CFLAGS='-O1 -g -std=gnu11 -Wall -Wextra -Werror -D_FILE_OFFSET_BITS=64 -ffunction-sections -fdata-sections -fno-omit-frame-pointer -fsanitize=address,undefined' LDFLAGS='-Wl,--gc-sections -fsanitize=address,undefined' test

# The Screensaver folder's rules: which files are pictures, judged from their
# JPEG headers, and the random pick between them. Plain C, no LVGL.
PICS_SRC = test_screensaverpics.c $(ROOT)/src/system/device/screensaverpics.c \
           $(ROOT)/src/system/library/jpeginfo.c
.PHONY: pictures
pictures: $(BUILD)/test_screensaverpics
	$(BUILD)/test_screensaverpics

$(BUILD)/test_screensaverpics: $(PICS_SRC) $(ROOT)/src/system/device/screensaverpics.h \
                               $(ROOT)/src/system/library/jpeginfo.h
	@mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $(PICS_SRC) $(LDFLAGS) $(LDLIBS)

# The software volume's arithmetic -- the whole level law for a USB DAC with
# no control of its own. Plain C, no ALSA: the three routing answers
# (Bluetooth, a card on the port, that card's control) are stubbed.
SWVOL_SRC = test_swvolume.c $(ROOT)/src/system/audio/swvolume.c
.PHONY: swvolume
swvolume: $(BUILD)/test_swvolume
	$(BUILD)/test_swvolume

$(BUILD)/test_swvolume: $(SWVOL_SRC) $(ROOT)/src/system/audio/swvolume.h \
                        $(ROOT)/src/system/audio/usbaudio.h $(ROOT)/src/system/audio/audio.h
	@mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $(SWVOL_SRC) $(LDFLAGS) $(LDLIBS)

# The Wi-Fi worker's hidden-network join: the real wifi.c against a fake
# wpa_cli on PATH (see test_wifi.c). Plain C, no LVGL and no radio.
WIFI_SRC = test_wifi.c $(ROOT)/src/system/net/wifi.c
.PHONY: wifi
wifi: $(BUILD)/test_wifi
	$(BUILD)/test_wifi

$(BUILD)/test_wifi: $(WIFI_SRC) $(ROOT)/src/system/net/wifi.h \
                    $(ROOT)/src/system/device/sysserver.h
	@mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) -Istubs $(CFLAGS) -o $@ test_wifi.c $(ROOT)/src/system/net/wifi.c $(LDFLAGS) $(LDLIBS)

# Optional real-codec integration: the usual host development packages are
# sufficient. No fake MP3/FLAC/Opus decoder is used. See tests/README.md.
CODEC = $(filter-out %/wavpackdec.c %/opusdec.c,$(wildcard $(ROOT)/src/system/decode/*.c)) \
        $(ROOT)/src/system/decode/opusdec.c $(ROOT)/src/system/decode/wavpackdec.c \
        $(ROOT)/src/system/decode/alac/alac.c $(ROOT)/src/system/decode/ape/apecore.c \
        $(ROOT)/src/system/library/cue.c
.PHONY: codecs sanitize-codecs
codecs: $(BUILD)/test_codecs
	python3 run_subsonic_tests.py $(BUILD)/test_codecs

$(BUILD)/test_codecs: test_codecs.c support.c $(CORE) $(CODEC)
	@mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -DTEST_REAL_CODECS=1 $(shell $(PKG_CONFIG) --cflags opusfile wavpack) \
		-o $@ test_codecs.c support.c $(CORE) $(CODEC) $(LDFLAGS) $(LDLIBS) $(shell $(PKG_CONFIG) --static --libs opusfile wavpack)

# Existing MP4 tag copies intentionally truncate display strings; GCC -O1
# diagnoses those inlined bounded copies. Keep all other warnings fatal.
sanitize-codecs:
	$(MAKE) BUILD=.build/sanitize-codecs CFLAGS='-O1 -g -std=gnu11 -Wall -Wextra -Werror -Wno-error=format-truncation -D_FILE_OFFSET_BITS=64 -ffunction-sections -fdata-sections -fno-omit-frame-pointer -fsanitize=address,undefined' LDFLAGS='-Wl,--gc-sections -fsanitize=address,undefined' codecs

clean:
	rm -rf $(BUILD)

# Optional headless LVGL layout regression tests. Reuse the production config,
# disabling SDL, FreeType and device drivers; no audio service is linked.
UI_SRC = $(ROOT)/tests/ui/test_main_menu.c $(ROOT)/src/gui/shell/main_menu.c \
         $(ROOT)/src/gui/library/music.c $(ROOT)/src/gui/shell/popover.c \
         $(ROOT)/src/gui/shell/gridpage.c $(ROOT)/src/gui/settings/appearance.c \
         $(ROOT)/src/gui/shell/settingsrow.c $(ROOT)/src/gui/shell/theme.c \
         $(ROOT)/src/gui/shell/icons.c $(ROOT)/src/gui/shell/main_menu_icons.c \
         $(ROOT)/src/gui/board_profile.c \
         $(ROOT)/src/system/core/config.c $(ROOT)/src/system/core/lang.c
UI_LVGL_SRC = $(shell find $(ROOT)/lvgl/src -name '*.c' 2>/dev/null)
UI_OBJ = $(patsubst $(ROOT)/%.c,$(BUILD)/ui/%.o,$(UI_SRC)) \
         $(patsubst $(ROOT)/%.c,$(BUILD)/ui/%.o,$(UI_LVGL_SRC))
UI_CPPFLAGS = $(CPPFLAGS) -I$(ROOT)/lvgl -DLV_CONF_PATH='"$(abspath ui/lv_conf.h)"'
UI_CFLAGS = $(CFLAGS) -MMD -MP

# The screensaver's Framed style on the same renderer; shares the objects above.
# The Wi-Fi page and its manual-entry page for hidden networks, on the same
# renderer; the radio under it is a recording stub (see test_wifi_pages.c).
WIFIPAGE_SRC = $(ROOT)/tests/ui/test_wifi_pages.c $(ROOT)/src/gui/wireless/wifisettings.c \
            $(ROOT)/src/gui/shell/settingsrow.c $(ROOT)/src/gui/shell/keyboard.c \
            $(ROOT)/src/gui/shell/popover.c $(ROOT)/src/gui/shell/spinner.c \
            $(ROOT)/src/gui/shell/theme.c $(ROOT)/src/gui/shell/icons.c \
            $(ROOT)/src/gui/board_profile.c $(ROOT)/src/system/input/kblayout.c \
            $(ROOT)/src/system/core/config.c $(ROOT)/src/system/core/lang.c
WIFIPAGE_OBJ = $(patsubst $(ROOT)/%.c,$(BUILD)/ui/%.o,$(WIFIPAGE_SRC)) \
            $(patsubst $(ROOT)/%.c,$(BUILD)/ui/%.o,$(UI_LVGL_SRC))

SAVER_SRC = $(ROOT)/tests/ui/test_screensaver.c $(ROOT)/src/gui/settings/screensaver.c \
            $(ROOT)/src/gui/shell/scrolltext.c $(ROOT)/src/gui/shell/settingsrow.c \
            $(ROOT)/src/gui/shell/theme.c $(ROOT)/src/gui/shell/icons.c \
            $(ROOT)/src/gui/board_profile.c \
            $(ROOT)/src/system/core/config.c $(ROOT)/src/system/core/lang.c
SAVER_OBJ = $(patsubst $(ROOT)/%.c,$(BUILD)/ui/%.o,$(SAVER_SRC)) \
            $(patsubst $(ROOT)/%.c,$(BUILD)/ui/%.o,$(UI_LVGL_SRC))

.PHONY: ui
ui: $(BUILD)/test_main_menu $(BUILD)/test_screensaver $(BUILD)/test_wifi_pages
	BOARD=tempotec_v1 $(BUILD)/test_main_menu ../../sonix-packer/assets/V1/usr/resource/morpthis/language
	BOARD=tempotec_v1 $(BUILD)/test_main_menu ../../sonix-packer/assets/V1/usr/resource/morpthis/language saved
	BOARD=tempotec_v1 $(BUILD)/test_screensaver ../../sonix-packer/assets/V1/usr/resource/morpthis/language
	BOARD=tempotec_v1 $(BUILD)/test_screensaver ../../sonix-packer/assets/V1/usr/resource/morpthis/language saved
	BOARD=tempotec_v1 $(BUILD)/test_wifi_pages ../../sonix-packer/assets/V1/usr/resource/morpthis/language

$(BUILD)/test_main_menu: $(UI_OBJ)
	$(CC) -o $@ $^ $(LDFLAGS) $(LDLIBS)

$(BUILD)/test_screensaver: $(SAVER_OBJ)
	$(CC) -o $@ $^ $(LDFLAGS) $(LDLIBS)

$(BUILD)/test_wifi_pages: $(WIFIPAGE_OBJ)
	$(CC) -o $@ $^ $(LDFLAGS) $(LDLIBS)

$(BUILD)/ui/%.o: $(ROOT)/%.c ui/lv_conf.h $(ROOT)/lv_conf.h
	@mkdir -p $(dir $@)
	$(CC) $(UI_CPPFLAGS) $(UI_CFLAGS) -c $< -o $@

# Upstream LVGL is not maintained here; keep its warnings nonfatal.
$(BUILD)/ui/lvgl/%.o: $(ROOT)/lvgl/%.c ui/lv_conf.h $(ROOT)/lv_conf.h
	@mkdir -p $(dir $@)
	$(CC) $(UI_CPPFLAGS) $(filter-out -Werror,$(UI_CFLAGS)) -c $< -o $@

-include $(sort $(UI_OBJ:.o=.d) $(SAVER_OBJ:.o=.d) $(WIFIPAGE_OBJ:.o=.d))
