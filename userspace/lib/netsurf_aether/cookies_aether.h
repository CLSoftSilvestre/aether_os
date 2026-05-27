#pragma once
#include <stddef.h>

#define COOKIE_MAX   256
#define COOKIE_FILE  "/config/cookies.txt"

/* Load cookies from /config/cookies.txt. Silent no-op if file absent. */
void cookies_init(void);

/* Parse a Set-Cookie: header value and store the cookie.
 * req_host  — hostname the request was made to (default domain when absent)
 * req_path  — request path (default path when absent)
 */
void cookie_set_from_header(const char *header_val,
                             const char *req_host,
                             const char *req_path);

/* Build the Cookie: header value for a request to host + path.
 * Writes "name=value; name2=value2" (no trailing semicolon) into out.
 * out[0] == '\0' if no cookies match.
 */
void cookies_build_header(const char *host, const char *path,
                           char *out, size_t out_max);
