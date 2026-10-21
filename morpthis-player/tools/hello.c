// The sideloading spike's test app. Built by `make hello`, copied to
// <card>/Apps/hello, and run from Developer options -> Run app from card.
//
// Static, so it depends on nothing in the rootfs: the device's glibc is 2.22
// and the toolchain's is 2.27, which a static link sidesteps entirely.
//
// The exit status is the answer the player reports. 42 here, so a toast saying
// "hello: exit 42" is the proof that execve() on the card worked and the
// status came back.

#include <stdio.h>

int main(void) {
	printf("hello from the microSD card\n");
	return 42;
}
