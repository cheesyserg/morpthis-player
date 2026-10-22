#include "sdk_internal.h"

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "src/system/device/system.h"

// The package is a read-only squashfs mount; the data folder is where an app
// may write. A relative path names the same file to the app either way:
//
//   read          the data folder first, then the package
//   write/append  the data folder, copying a package file there first when the
//                 app opens one for update (copy-on-write)
//
// So a shipped default file can be edited without touching the image, and the
// package stays the one file a user deletes to uninstall.

struct morpthis_file {
	FILE *fp;
};

static const char *package_folder(void) {
	return sdk_app ? sdk_app->folder : "";
}

static const char *data_folder(void) {
	if (sdk_app && sdk_app->data[0]) {
		return sdk_app->data;
	}
	return package_folder();
}

static bool path_exists(const char *path) {
	struct stat st;
	return path && path[0] && stat(path, &st) == 0;
}

// Makes the directories a relative path needs under `base`. Used before a
// package file is copied into the data folder: the subdirectory it lived in
// may exist only in the image.
static int make_parents(const char *base, const char *relative) {
	char path[APPS_FOLDER_MAX + 256];
	int n = snprintf(path, sizeof(path), "%s/%s", base, relative);
	if (n < 0 || (size_t)n >= sizeof(path)) {
		errno = ENAMETOOLONG;
		return -1;
	}

	size_t base_length = strlen(base);
	for (char *p = path + base_length + 1; *p; p++) {
		if (*p != '/') {
			continue;
		}
		*p = '\0';
		if (mkdir(path, 0755) != 0 && errno != EEXIST) {
			return -1;
		}
		*p = '/';
	}
	return 0;
}

static bool copy_file(const char *from, const char *to) {
	if (strcmp(from, to) == 0) {
		return true; // host runtime: the data folder is the app folder
	}

	char buffer[4096];
	FILE *in = fopen(from, "rb");
	if (!in) {
		return false;
	}
	FILE *out = fopen(to, "wb");
	if (!out) {
		fclose(in);
		return false;
	}

	bool ok = true;
	size_t got;
	while ((got = fread(buffer, 1, sizeof(buffer), in)) > 0) {
		if (fwrite(buffer, 1, got, out) != got) {
			ok = false;
			break;
		}
	}
	if (ferror(in)) {
		ok = false;
	}
	if (fclose(out) != 0) {
		ok = false;
	}
	fclose(in);

	if (!ok) {
		remove(to);
		if (!errno) {
			errno = EIO;
		}
	}
	return ok;
}

// Copies a package file into the data folder so it can be opened for update.
static bool copy_package_file(const char *relative, const char *package_path,
							  const char *data_path) {
	if (make_parents(data_folder(), relative) != 0) {
		return false;
	}
	return copy_file(package_path, data_path);
}

const char *morpthis_fs_app_folder(void) {
	return package_folder();
}

const char *morpthis_fs_data_folder(void) {
	return data_folder();
}

const char *morpthis_fs_card_root(void) {
	const char *root = storage_sd_root();
	return root ? root : "";
}

bool morpthis_fs_resolve(const char *relative, char *out, size_t size) {
	char path[APPS_FOLDER_MAX + 256];
	if (!out || size == 0) {
		errno = EINVAL;
		return false;
	}

	if (sdk_resolve_in(data_folder(), relative, path, sizeof(path)) != 0) {
		return false;
	}
	if (!path_exists(path)) {
		if (!sdk_app || !sdk_app->folder[0]) {
			errno = ENOENT;
			return false;
		}
		if (sdk_resolve_in(sdk_app->folder, relative, path, sizeof(path)) != 0) {
			return false;
		}
		if (!path_exists(path)) {
			errno = ENOENT;
			return false;
		}
	}

	int n = snprintf(out, size, "%s", path);
	if (n < 0 || (size_t)n >= size) {
		errno = ENAMETOOLONG;
		return false;
	}
	return true;
}

morpthis_file_t *morpthis_fs_open(const char *relative, morpthis_fs_mode_t mode) {
	char data_path[APPS_FOLDER_MAX + 256];
	char package_path[APPS_FOLDER_MAX + 256];

	const char *base = data_folder();
	if (sdk_resolve_in(base, relative, data_path, sizeof(data_path)) != 0) {
		return NULL;
	}

	bool in_data = path_exists(data_path);
	const char *chosen = data_path;

	switch (mode) {
	case MORPTHIS_FS_READ:
		if (!in_data) {
			if (sdk_resolve_in(package_folder(), relative, package_path,
							   sizeof(package_path)) != 0) {
				return NULL;
			}
			if (!path_exists(package_path)) {
				errno = ENOENT;
				return NULL;
			}
			chosen = package_path;
		}
		break;

	case MORPTHIS_FS_WRITE:
		break;

	case MORPTHIS_FS_APPEND:
	case MORPTHIS_FS_READ_WRITE:
		if (!in_data && sdk_app && sdk_app->folder[0] &&
			sdk_resolve_in(sdk_app->folder, relative, package_path,
						   sizeof(package_path)) == 0 &&
			path_exists(package_path) &&
			!copy_package_file(relative, package_path, data_path)) {
			return NULL;
		}
		break;

	default:
		errno = EINVAL;
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

	FILE *fp = fopen(chosen, flags);
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
	if (sdk_resolve_in(data_folder(), relative, path, sizeof(path)) == 0 &&
		path_exists(path)) {
		return true;
	}
	if (sdk_app && sdk_app->folder[0] &&
		sdk_resolve_in(sdk_app->folder, relative, path, sizeof(path)) == 0) {
		return path_exists(path);
	}
	return false;
}

int morpthis_fs_remove(const char *relative) {
	char data_path[APPS_FOLDER_MAX + 256];
	char package_path[APPS_FOLDER_MAX + 256];

	if (sdk_resolve_in(data_folder(), relative, data_path, sizeof(data_path)) != 0) {
		return -1;
	}
	if (remove(data_path) == 0) {
		return 0;
	}
	if (errno != ENOENT) {
		return -1;
	}

	// Removing a shipped file would change the package, which is read-only.
	if (sdk_app && sdk_app->folder[0] &&
		sdk_resolve_in(sdk_app->folder, relative, package_path,
					   sizeof(package_path)) == 0 &&
		path_exists(package_path)) {
		errno = EROFS;
	}
	return -1;
}

int morpthis_fs_rename(const char *from, const char *to) {
	char from_path[APPS_FOLDER_MAX + 256];
	char to_path[APPS_FOLDER_MAX + 256];

	const char *base = data_folder();
	if (sdk_resolve_in(base, from, from_path, sizeof(from_path)) != 0 ||
		sdk_resolve_in(base, to, to_path, sizeof(to_path)) != 0) {
		return -1;
	}

	if (rename(from_path, to_path) == 0) {
		return 0;
	}
	if (errno != ENOENT) {
		return -1;
	}

	char package_path[APPS_FOLDER_MAX + 256];
	if (sdk_app && sdk_app->folder[0] &&
		sdk_resolve_in(sdk_app->folder, from, package_path,
					   sizeof(package_path)) == 0 &&
		path_exists(package_path)) {
		errno = EROFS;
	}
	return -1;
}

int morpthis_fs_mkdir(const char *relative) {
	char data_path[APPS_FOLDER_MAX + 256];
	if (sdk_resolve_in(data_folder(), relative, data_path, sizeof(data_path)) != 0) {
		return -1;
	}
	if (path_exists(data_path)) {
		errno = EEXIST;
		return -1;
	}

	// A directory that exists only in the package is visible to reads, but it
	// is still read-only. Creating the data one shadows it and gives the app a
	// place to add files, which is what mkdir is asking for here.
	return mkdir(data_path, 0755);
}

// ---------------------------------------------------------------------------
// Full access
// ---------------------------------------------------------------------------

#define SDK_ABS_PATH_MAX 1024

static const char *mode_flags(morpthis_fs_mode_t mode) {
	switch (mode) {
	case MORPTHIS_FS_READ:
		return "rb";
	case MORPTHIS_FS_WRITE:
		return "wb";
	case MORPTHIS_FS_APPEND:
		return "ab";
	case MORPTHIS_FS_READ_WRITE:
		return "r+b";
	default:
		return NULL;
	}
}

morpthis_file_t *morpthis_fs_open_absolute(const char *path, morpthis_fs_mode_t mode) {
	if (!path || path[0] != '/') {
		errno = EINVAL;
		return NULL;
	}
	const char *flags = mode_flags(mode);
	if (!flags) {
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

bool morpthis_fs_exists_absolute(const char *path) {
	return path && path[0] == '/' && path_exists(path);
}

bool morpthis_fs_write_all_absolute(const char *path, const void *buffer, size_t size) {
	morpthis_file_t *file = morpthis_fs_open_absolute(path, MORPTHIS_FS_WRITE);
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

long morpthis_fs_read_all_absolute(const char *path, char *buffer, size_t max, size_t *out_size) {
	if (!buffer || max == 0) {
		errno = EINVAL;
		return -1;
	}
	morpthis_file_t *file = morpthis_fs_open_absolute(path, MORPTHIS_FS_READ);
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

int morpthis_fs_remove_absolute(const char *path) {
	if (!path || path[0] != '/') {
		errno = EINVAL;
		return -1;
	}
	return remove(path);
}

int morpthis_fs_rmdir_absolute(const char *path) {
	if (!path || path[0] != '/') {
		errno = EINVAL;
		return -1;
	}
	return rmdir(path);
}

int morpthis_fs_rename_absolute(const char *from, const char *to) {
	if (!from || !to || from[0] != '/' || to[0] != '/') {
		errno = EINVAL;
		return -1;
	}
	return rename(from, to);
}

int morpthis_fs_mkdir_absolute(const char *path) {
	if (!path || path[0] != '/') {
		errno = EINVAL;
		return -1;
	}
	return mkdir(path, 0755);
}

bool morpthis_fs_stat_absolute(const char *path, morpthis_fs_stat_t *out) {
	if (!path || path[0] != '/' || !out) {
		errno = EINVAL;
		return false;
	}
	struct stat st;
	if (stat(path, &st) != 0) {
		return false;
	}
	out->size = (uint64_t)st.st_size;
	out->mtime = (int64_t)st.st_mtime;
	out->is_dir = S_ISDIR(st.st_mode);
	return true;
}

struct morpthis_dir {
	DIR *dir;
	char base[SDK_ABS_PATH_MAX];
	char name[256];
	bool is_dir;
};

morpthis_dir_t *morpthis_fs_open_dir(const char *absolute_path) {
	if (!absolute_path || absolute_path[0] != '/') {
		errno = EINVAL;
		return NULL;
	}
	DIR *dir = opendir(absolute_path);
	if (!dir) {
		return NULL;
	}
	morpthis_dir_t *handle = calloc(1, sizeof(*handle));
	if (!handle) {
		closedir(dir);
		return NULL;
	}
	handle->dir = dir;
	snprintf(handle->base, sizeof(handle->base), "%s", absolute_path);
	return handle;
}

const char *morpthis_fs_dir_next(morpthis_dir_t *handle) {
	if (!handle || !handle->dir) {
		return NULL;
	}
	struct dirent *entry;
	while ((entry = readdir(handle->dir)) != NULL) {
		if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
			continue;
		}
		snprintf(handle->name, sizeof(handle->name), "%s", entry->d_name);
		char path[SDK_ABS_PATH_MAX + sizeof(handle->name)];
		if (snprintf(path, sizeof(path), "%s/%s", handle->base, handle->name) < (int)sizeof(path)) {
			struct stat st;
			handle->is_dir = stat(path, &st) == 0 && S_ISDIR(st.st_mode);
		} else {
			handle->is_dir = false;
		}
		return handle->name;
	}
	return NULL;
}

bool morpthis_fs_dir_is_dir(const morpthis_dir_t *handle) {
	return handle ? handle->is_dir : false;
}

void morpthis_fs_dir_close(morpthis_dir_t *handle) {
	if (!handle) {
		return;
	}
	if (handle->dir) {
		closedir(handle->dir);
	}
	free(handle);
}
