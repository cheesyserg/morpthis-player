#include "sdk_internal.h"

#include <stdlib.h>
#include <string.h>

#include "src/system/net/http.h"

// ---------------------------------------------------------------------------
// net.h
// ---------------------------------------------------------------------------

#define DEFAULT_LIMIT (1024u * 1024u)
#define DEFAULT_TIMEOUT_SECS 15

static size_t effective_limit(size_t limit) { return limit ? limit : DEFAULT_LIMIT; }

static int effective_timeout(int timeout_secs) { return timeout_secs > 0 ? timeout_secs : DEFAULT_TIMEOUT_SECS; }

bool morpthis_http_get(const char *url, const char *extra_headers, bool want_error_body,
					   size_t limit, int timeout_secs, morpthis_http_response_t *out) {
	if (!out) {
		return false;
	}
	memset(out, 0, sizeof(*out));
	if (!url || !url[0]) {
		return false;
	}

	char *body = NULL;
	size_t length = 0;
	int status = 0;
	if (!http_get_ex(url, extra_headers, want_error_body, &body, &length,
					 effective_limit(limit), effective_timeout(timeout_secs), &status)) {
		return false;
	}
	out->body = body;
	out->length = length;
	out->status = status;
	return true;
}

bool morpthis_http_perform(const char *url, const morpthis_http_opts_t *opts,
						   morpthis_http_response_t *out) {
	if (!out) {
		return false;
	}
	memset(out, 0, sizeof(*out));
	if (!url || !url[0] || !opts) {
		return false;
	}

	http_req_t req;
	memset(&req, 0, sizeof(req));
	req.method = opts->method;
	req.extra_headers = opts->extra_headers;
	req.body = opts->body;
	req.body_len = opts->body_len;
	req.content_type = opts->content_type;
	req.want_error_body = opts->want_error_body;
	req.allow_empty_body = opts->allow_empty_body;
	req.no_https_downgrade = opts->no_https_downgrade;

	char *body = NULL;
	size_t length = 0;
	int status = 0;
	if (!http_request(url, &req, &body, &length, effective_limit(opts->limit),
					  effective_timeout(opts->timeout_secs), &status)) {
		return false;
	}
	out->body = body;
	out->length = length;
	out->status = status;
	return true;
}

void morpthis_http_response_free(morpthis_http_response_t *response) {
	if (!response) {
		return;
	}
	free(response->body);
	response->body = NULL;
	response->length = 0;
	response->status = 0;
}

const char *morpthis_http_last_error(void) {
	return http_last_error();
}

char *morpthis_http_url_encode(const char *in, char *out, size_t size) {
	return http_url_encode(in, out, size);
}
