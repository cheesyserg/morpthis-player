#ifndef RESPATH_H
#define RESPATH_H

// Root of the resource tree. Absolute on the device, relative to the working
// directory on the host.
#ifdef HOST_BUILD
#define RESOURCE_DIR "usr/resource"
#else
#define RESOURCE_DIR "/usr/resource"
#endif

#define MORPTHIS_RESOURCE_DIR RESOURCE_DIR "/morpthis"

#endif /* RESPATH_H */
