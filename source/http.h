#ifndef HTTP_H
#define HTTP_H

/*
 * Minimal blocking HTTP/1.1 client for dswifi. Plain HTTP only
 * (see config.h).
 */

/* Progress callback; done/total in bytes. total<0 if unknown. */
typedef void (*http_progress_t)(long done, long total);

/*
 * GET <path> and stream the body to memory.
 * Returns number of body bytes written to buf (<= max), or -1 on error.
 * A progress callback (optional) is invoked periodically.
 */
long http_get_to_mem(const char *path, unsigned char *buf, long max,
                     http_progress_t progress);

/*
 * GET <path> and stream the body to a file.
 * Returns 0 on success, -1 on error.
 */
int http_get_to_file(const char *path, const char *fatfile,
                     http_progress_t progress);

/*
 * Override the remote server host/port at runtime. Defaults come from
 * config.h macros; called with the values from the SD config file.
 */
void http_set_server(const char *host, unsigned short port);

/* Current configured host / port (for display). */
const char *http_get_host(void);
unsigned short http_get_port(void);

/*
 * Diagnostics from the most recent request (for the UI):
 *  http_last_status : the HTTP status code (0 if not received)
 *  http_last_html   : 1 if the server answered with a text/html page
 *                     (e.g. an ngrok interstitial) instead of the file
 */
extern int http_last_status;
extern int http_last_html;

#endif