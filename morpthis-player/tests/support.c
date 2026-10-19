// Only platform helpers/library indexes are stubbed. HTTP/TLS, REST, JSON,
// authentication, configuration, ring buffers and source leases are real code.
#include "src/system/core/utils.h"
#include "src/system/core/lang.h"
#include "src/system/library/library.h"
#include "src/system/library/cue.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

const char *tr(const char *key) { return key; }
void thread_be_low_priority(const char *name) { (void)name; }
void thread_be_background(const char *name) { (void)name; }
void thread_signal_stack(void) {}

bool has_extension(const char *path, const char *ext) {
	size_t p = strlen(path), e = strlen(ext);
	return p >= e && strcasecmp(path + p - e, ext) == 0;
}

long get_file_size(const char *path) {
	struct stat st;
	return stat(path, &st) == 0 ? (long)st.st_size : -1;
}

void library_queue_save_album(const char *album) { (void)album; }
void library_index_close(library_index_t *ix) { (void)ix; }
int library_index_count(const library_index_t *ix) { (void)ix; return 0; }
bool library_index_stale(const library_index_t *ix) { (void)ix; return false; }
void library_index_describe(const library_index_t *ix, library_index_spec_t *out) {
	(void)ix;
	memset(out, 0, sizeof(*out));
}
library_index_t *library_index_open(library_list_t kind, library_filter_t filter, const char *value,
									library_order_t order, bool desc) {
	(void)kind; (void)filter; (void)value; (void)order; (void)desc;
	return NULL;
}
int library_index_find_path(const library_index_t *ix, const char *path) { (void)ix; (void)path; return -1; }
int library_index_window(const library_index_t *ix, int offset, int count, library_row_cb cb, void *user) {
	(void)ix; (void)offset; (void)count; (void)cb; (void)user;
	return 0;
}

#ifndef TEST_REAL_CODECS
bool cue_order_tracks(const char *a, const char *b, int *order) { (void)a; (void)b; (void)order; return false; }
#endif
