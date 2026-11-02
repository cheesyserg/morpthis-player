#include "led_i2c.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "src/system/core/config.h"

// The SGM31324 sits on I2C bus 0 at address 0x30. The register map is the one
// in docs/sgm31324-led-control.md; registers 0x00 and 0x03 are held at zero and
// written only by the breathing routine, which programs every register it
// depends on.
#define I2C_BUS "/dev/i2c-0"
#define I2C_ADDR 0x30

// From <linux/i2c-dev.h>. Defined here so the build does not depend on the
// kernel headers being installed for the cross toolchain.
#define I2C_SLAVE 0x0703

#define REG_PERIOD 0x01
#define REG_DUTY 0x02
#define REG_MAP 0x04
#define REG_FADE 0x05
#define REG_R 0x06
#define REG_G 0x07
#define REG_B 0x08
#define REG_MODE 0x09

// Bit 0x40 (DEV_EN) must always be set in the enable map.
#define DEV_EN 0x40

// The breathing-mode channel bits: the steady bits shifted up by one.
#define BREATHE_R 0x02
#define BREATHE_G 0x08
#define BREATHE_B 0x20

static int i2c_fd = -1;

// The kernel engine. KMOD_FILE is where the firmware packer installs it, next
// to the vendor's own modules; KMOD_DIR is the node the module creates.
#define KMOD_FILE "/module_driver/morpthis_led.ko"
#define KMOD_NAME "morpthis_led"
#define KMOD_DIR "/sys/devices/platform/morpthis-led"
// Written before the module is loaded and removed once it has answered. A
// boot that finds it still there is one whose previous load never finished --
// a kernel the module does not suit, which oopsed or hung on the way in -- and
// that boot stays on the I2C path rather than trying again. Removing the file
// (or a factory reset) asks for one more try.
#define KMOD_GUARD "/usr/data/.morpthis_led_loading"

static int kmod_render_fd = -1;
static int kmod_wait_fd = -1;
static bool kmod_guard_pending; // the guard stays until the engine has run a target

// Unloads the firmware driver with the syscall directly, so it does not depend
// on a shell or on rmmod being in the player's PATH. ENOENT means the module is
// already gone, which is just as good.
static int unload_driver(void) {
	errno = 0;
	long rc = syscall(SYS_delete_module, "leds_sgm31324_add", O_NONBLOCK);
	if (rc == 0 || errno == ENOENT) {
		return 0;
	}
	return errno ? errno : EIO;
}

static void write_reg(unsigned char reg, unsigned char value) {
	if (i2c_fd < 0) {
		return;
	}
	unsigned char buf[2] = {reg, value};
	if (write(i2c_fd, buf, sizeof(buf)) != (ssize_t)sizeof(buf)) {
		fprintf(stderr, "led: i2c write reg 0x%02x failed\n", reg);
	}
}

static bool kmod_open(void) {
	kmod_render_fd = open(KMOD_DIR "/render", O_WRONLY | O_CLOEXEC);
	if (kmod_render_fd < 0) {
		return false;
	}
	kmod_wait_fd = open(KMOD_DIR "/wait", O_RDONLY | O_CLOEXEC);
	if (kmod_wait_fd < 0) {
		close(kmod_render_fd);
		kmod_render_fd = -1;
		return false;
	}
	return true;
}

// Loads the engine with finit_module(2) directly, like the unload below, so
// nothing depends on insmod being in the player's PATH. EEXIST is a module a
// previous run of the player already loaded.
static bool kmod_load(void) {
	if (access(KMOD_DIR "/render", W_OK) == 0) {
		return kmod_open();
	}
	if (access(KMOD_FILE, R_OK) != 0) {
		return false; // an image without it
	}
	if (access(KMOD_GUARD, F_OK) == 0) {
		fprintf(stderr, "led: %s is still there from a load that never finished -- staying on i2c\n", KMOD_GUARD);
		return false;
	}

	int guard = open(KMOD_GUARD, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	if (guard >= 0) {
		fsync(guard);
		close(guard);
		sync();
	}

	bool ok = false;
#ifdef SYS_finit_module
	int fd = open(KMOD_FILE, O_RDONLY | O_CLOEXEC);
	if (fd >= 0) {
		errno = 0;
		long rc = syscall(SYS_finit_module, fd, "", 0);
		int err = errno;
		close(fd);
		if (rc == 0 || err == EEXIST) {
			ok = kmod_open();
		} else {
			fprintf(stderr, "led: %s will not load (%s) -- staying on i2c\n", KMOD_FILE, strerror(err));
		}
	}
#endif
	// A refusal is safe (nothing ran), so the guard goes now. A module that
	// came up keeps it until it has run its first target: the work it does
	// then is the part a mismatched kernel would trip over, and a crash there
	// has to leave the next boot on the I2C path too.
	if (ok) {
		kmod_guard_pending = true;
	} else {
		unlink(KMOD_GUARD);
	}
	return ok;
}

static void kmod_guard_clear(void) {
	if (kmod_guard_pending) {
		kmod_guard_pending = false;
		unlink(KMOD_GUARD);
	}
}

bool led_kmod_ready(void) { return kmod_render_fd >= 0; }

bool led_kmod_render(int mode, int r, int g, int b, int period, int duty, int fade, int transition_ms,
					 int step_ms) {
	if (kmod_render_fd < 0) {
		return false;
	}
	char line[96];
	int len = snprintf(line, sizeof(line), "%d %d %d %d %d %d %d %d %d\n", mode, r, g, b, period, duty, fade,
					   transition_ms, step_ms);
	if (len <= 0 || write(kmod_render_fd, line, (size_t)len) != len) {
		fprintf(stderr, "led: kernel engine refused a render (%s)\n", strerror(errno));
		return false;
	}
	if (mode == LED_KMOD_BREATHE || mode == LED_KMOD_BLINK) {
		kmod_guard_clear(); // written and started in the same write
	}
	return true;
}

int led_kmod_wait(void) {
	if (kmod_wait_fd < 0) {
		return -1;
	}
	char c = 0;
	if (pread(kmod_wait_fd, &c, 1, 0) != 1) {
		return -1;
	}
	if (c != '1') {
		kmod_guard_clear(); // a fade ran to its end
		return 0;
	}
	return 1;
}

bool led_i2c_init(void) {
#ifdef HOST_BUILD
	// The simulator has no SGM31324, and must never touch the developer's own
	// I2C buses.
	return false;
#endif
	if (i2c_fd >= 0 || kmod_render_fd >= 0) {
		return true;
	}

	// Open the bus first: if it is not there, the driver is left alone and the
	// caller keeps the pattern node. A missing /dev/i2c-0 almost always means the
	// i2c-dev module is not loaded, so try to bring it up before giving up.
	int fd = open(I2C_BUS, O_RDWR);
	if (fd < 0) {
		int rc = system("modprobe i2c-dev 2>/dev/null");
		(void)rc;
		fd = open(I2C_BUS, O_RDWR);
	}
	if (fd < 0) {
		fprintf(stderr, "led: %s will not open (%s) -- is the i2c-dev module loaded?\n", I2C_BUS, strerror(errno));
		return false;
	}
	if (ioctl(fd, I2C_SLAVE, I2C_ADDR) < 0) {
		fprintf(stderr, "led: %s: cannot select address 0x%02x (%s)\n", I2C_BUS, I2C_ADDR, strerror(errno));
		close(fd);
		return false;
	}

	// The controller is ours only once the firmware driver lets go. Unloading
	// does not reset the chip -- it keeps running on its last register values --
	// which is why every routine programs the registers it depends on. If the
	// unload fails the driver still owns the chip, so leave it in charge and let
	// the caller fall back to the registered patterns. EWOULDBLOCK here means the
	// module is in use; EPERM usually means it is built in, not loadable.
	int err = unload_driver();
	if (err != 0) {
		fprintf(stderr, "led: could not unload leds_sgm31324_add (%s) -- keeping the driver's patterns\n", strerror(err));
		close(fd);
		return false;
	}

	// The controller is free. The kernel engine takes it when the image has
	// it; the bus stays open either way, so a module that misbehaves later can
	// be dropped without losing the light.
	i2c_fd = fd;
	if (config_get_int("led", "kernel_driver", 1) != 0 && kmod_load()) {
		fprintf(stderr, "led: driven by the kernel engine (%s)\n", KMOD_NAME);
	}
	return true;
}

bool led_i2c_ready(void) { return i2c_fd >= 0; }

void led_i2c_levels(int r, int g, int b, unsigned channels) {
	unsigned char map = DEV_EN;
	if (channels & LED_CH_R) {
		map |= 0x01;
	}
	if (channels & LED_CH_G) {
		map |= 0x04;
	}
	if (channels & LED_CH_B) {
		map |= 0x10;
	}
	write_reg(REG_MAP, map);
	write_reg(REG_R, (unsigned char)r);
	write_reg(REG_G, (unsigned char)g);
	write_reg(REG_B, (unsigned char)b);
}

void led_i2c_steady(void) { write_reg(REG_MODE, 0x00); }

void led_i2c_breathe(int r, int g, int b, unsigned char period, unsigned char duty, unsigned char fade) {
	unsigned char map = DEV_EN;
	if (r > 0) {
		map |= BREATHE_R;
	}
	if (g > 0) {
		map |= BREATHE_G;
	}
	if (b > 0) {
		map |= BREATHE_B;
	}
	write_reg(0x00, 0x00);
	write_reg(REG_PERIOD, period);
	write_reg(REG_DUTY, duty);
	write_reg(0x03, 0x00);
	write_reg(REG_MAP, map);
	write_reg(REG_FADE, fade);
	write_reg(REG_R, (unsigned char)r);
	write_reg(REG_G, (unsigned char)g);
	write_reg(REG_B, (unsigned char)b);
	write_reg(REG_MODE, 0x06);
}

void led_i2c_off(void) {
	// The enable map off entirely, as the doc's fade_out leaves it: DEV_EN too,
	// so the controller draws nothing until the next colour sets it again.
	write_reg(REG_MAP, 0x00);
	write_reg(REG_R, 0x00);
	write_reg(REG_G, 0x00);
	write_reg(REG_B, 0x00);
	write_reg(REG_MODE, 0x00);
}
