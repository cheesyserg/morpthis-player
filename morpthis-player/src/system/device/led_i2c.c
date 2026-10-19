#include "led_i2c.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <unistd.h>

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

bool led_i2c_init(void) {
#ifdef HOST_BUILD
	// The simulator has no SGM31324, and must never touch the developer's own
	// I2C buses.
	return false;
#endif
	if (i2c_fd >= 0) {
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

	i2c_fd = fd;
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
