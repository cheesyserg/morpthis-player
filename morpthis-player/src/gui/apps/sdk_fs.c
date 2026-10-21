#include "sdk_internal.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "src/system/device/system.h"

struct morpthis_file {
	FILE *fp;
};

const char *morpthis_fs_app_folder(void) {
	return sdk_app ? sdk_app->folder : "";
}

const char *morpthis_fs_card_root(void) {
	const char *root = storage_sd_root();
	return root ? root : "";
}

morpthis_file_t *morpthis_fs_open(const char *relative, morpthis_fs_mode_t mode) {
	char path[APPS_FOLDER_MAX + 256];
	if (sdk_resolve_path(relative, path, sizeof(path)) != 0) {
		return NULL;
	}

	const char *flags;
	switch (mode) {
	case MORPTHIS_FS_READ:
		flags = "rb";
		break;
	case MORPTHIS_FS_WRITE:
		flags = "wb";
		break;
	case MORPTHIS_FS_APPEND:
		flags = "ab";
		break;
	case MORPTHIS_FS_READ_WRITE:
		flags = "r+b";
		break;
	default:
		errno = EINVAL;
		return NULL;
	}

	FILE *fp = fopen(path, flags);
	if (!fp) {
		return NULL;
	}
	morpthis_file_t *file = calloc(1, sizeof(*file));
	if (!file) {
		fclose(fp);
		return NULL;
	}
	file->fp = fp;
	return file;
}

size_t morpthis_fs_read(morpthis_file_t *file, void *buffer, size_t size) {
	if (!file || !file->fp || (!buffer && size > 0)) {
		errno = EINVAL;
		return 0;
	}
	return fread(buffer, 1, size, file->fp);
}

size_t morpthis_fs_write(morpthis_file_t *file, const void *buffer, size_t size) {
	if (!file || !file->fp || (!buffer && size > 0)) {
		errno = EINVAL;
		return 0;
	}
	return fwrite(buffer, 1, size, file->fp);
}

int morpthis_fs_seek(morpthis_file_t *file, long offset, int whence) {
	if (!file || !file->fp) {
		errno = EINVAL;
		return -1;
	}
	return fseek(file->fp, offset, whence);
}

long morpthis_fs_tell(morpthis_file_t *file) {
	if (!file || !file->fp) {
		errno = EINVAL;
		return -1;
	}
	return ftell(file->fp);
}

int morpthis_fs_close(morpthis_file_t *file) {
	if (!file) {
		errno = EINVAL;
		return -1;
	}
	int rc = file->fp ? fclose(file->fp) : 0;
	free(file);
	return rc;
}

bool morpthis_fs_write_all(const char *relative, const void *buffer, size_t size) {
	morpthis_file_t *file = morpthis_fs_open(relative, MORPTHIS_FS_WRITE);
	if (!file) {
		return false;
	}
	size_t written = morpthis_fs_write(file, buffer, size);
	int close_rc = morpthis_fs_close(file);
	if (written != size) {
		if (!errno) {
			errno = EIO;
		}
		return false;
	}
	return close_rc == 0;
}

long morpthis_fs_read_all(const char *relative, char *buffer, size_t max,
						  size_t *out_size) {
	if (!buffer || max == 0) {
		errno = EINVAL;
		return -1;
	}

	morpthis_file_t *file = morpthis_fs_open(relative, MORPTHIS_FS_READ);
	if (!file) {
		return -1;
	}

	size_t total = 0;
	while (total < max - 1) {
		size_t got = morpthis_fs_read(file, buffer + total, max - 1 - total);
		total += got;
		if (got == 0) {
			break;
		}
	}
	bool failed = ferror(file->fp) != 0;
	int close_rc = morpthis_fs_close(file);
	if (out_size) {
		*out_size = total;
	}
	buffer[total] = '\0';

	if (failed) {
		errno = EIO;
		return -1;
	}
	if (close_rc != 0) {
		return -1;
	}
	return (long)total;
}

bool morpthis_fs_exists(const char *relative) {
	char path[APPS_FOLDER_MAX + 256];
	if (sdk_resolve_path(relative, path, sizeof(path)) != 0) {
		return false;
	}
	struct stat st;
	return stat(path, &st) == 0;
}

int morpthis_fs_remove(const char *relative) {
	char path[APPS_FOLDER_MAX + 256];
	if (sdk_resolve_path(relative, path, sizeof(path)) != 0) {
		return -1;
	}
	return remove(path);
}

int morpthis_fs_rename(const char *from, const char *to) {
	char from_path[APPS_FOLDER_MAX + 256];
	char to_path[APPS_FOLDER_MAX + 256];
	if (sdk_resolve_path(from, from_path, sizeof(from_path)) != 0 ||
		sdk_resolve_path(to, to_path, sizeof(to_path)) != 0) {
		return -1;
	}
	return rename(from_path, to_path);
}

int morpthis_fs_mkdir(const char *relative) {
	char path[APPS_FOLDER_MAX + 256];
	if (sdk_resolve_path(relative, path, sizeof(path)) != 0) {
		return -1;
	}
	return mkdir(path, 0755);
}
