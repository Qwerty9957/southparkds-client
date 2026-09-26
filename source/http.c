#include <nds.h>
#include <dswifi9.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>

#include <string.h>
#include <stdio.h>
#include "config.h"
#include "http.h"

/* ------------------------------------------------------------------ */
/* runtime server host/port (overridable via http_set_server)          */
/* ------------------------------------------------------------------ */
static char rhost[128] = HTTP_HOST;
static unsigned short rport = HTTP_PORT;

int http_last_status = 0;
int http_last_html = 0;

void http_set_server(const char *host, unsigned short port) {
	if (host) {
		strncpy(rhost, host, sizeof rhost - 1);
		rhost[sizeof rhost - 1] = 0;
	}
	if (port > 0) rport = port;
}

const char *http_get_host(void) { return rhost; }
unsigned short http_get_port(void) { return rport; }

/* ------------------------------------------------------------------ */
/* small buffered reader over the socket                              */
/* ------------------------------------------------------------------ */
#define RBUF 1024
static char rbuf[RBUF];
static int rpos, rlen;

static int refill(int s) {
	rpos = 0;
	int r = recv(s, rbuf, RBUF, 0);
	if (r <= 0) return 0;
	rlen = r;
	return rlen;
}

static int bgetc(int s) {
	if (rpos >= rlen) {
		if (!refill(s)) return -1;
	}
	return (unsigned char)rbuf[rpos++];
}

/* ------------------------------------------------------------------ */
static int connect_http(void) {
	struct hostent *h = gethostbyname(rhost);
	if (!h) return -1;

	int s = socket(AF_INET, SOCK_STREAM, 0);
	if (s < 0) return -1;

	struct sockaddr_in a;
	memset(&a, 0, sizeof a);
	a.sin_family = AF_INET;
	a.sin_port = htons(rport);
	memcpy(&a.sin_addr.s_addr, h->h_addr_list[0], 4);

	if (connect(s, (struct sockaddr *)&a, sizeof a) != 0) {
		closesocket(s);
		return -1;
	}
	rpos = rlen = 0;
	return s;
}

static int send_request(int s, const char *path) {
	char req[512];
	int n = snprintf(req, sizeof req,
		"GET %s HTTP/1.1\r\n"
		"Host: %s\r\n"
		"User-Agent: SouthparkDS/1.0\r\n"
		"ngrok-skip-browser-warning: true\r\n"
		"Accept: */*\r\n"
		"Connection: close\r\n"
		"\r\n", path, rhost);
	int off = 0;
	while (off < n) {
		int w = send(s, req + off, n - off, 0);
		if (w <= 0) return -1;
		off += w;
	}
	return 0;
}

/* ------------------------------------------------------------------ */
/* returns 0 if HTTP 2xx and content length >= 0, -1 otherwise.       */
/* sets *cl to Content-Length (or -1 when unknown).                   */
/* Any body bytes already received stay in the buffer for bgetc.      */
/* ------------------------------------------------------------------ */
static int read_headers(int s, long *cl) {
	char hdr[2048];
	int hl = 0, status = 0;
	char c0 = 0, c1 = 0, c2 = 0, c3 = 0; /* last 4 bytes, c3 = newest */
	*cl = -1;
	http_last_status = 0;
	http_last_html = 0;

	while (hl < (int)sizeof(hdr) - 1) {
		int c = bgetc(s);
		if (c < 0) break;
		c0 = c1; c1 = c2; c2 = c3; c3 = (char)c;
		hdr[hl++] = (char)c;
		/* CRLF CRLF : c0 c1 c2 c3 == '\r' '\n' '\r' '\n' */
		if (c3 == '\n' && c2 == '\r' && c1 == '\n' && c0 == '\r')
			break;
	}
	hdr[hl] = 0;

	if (sscanf(hdr, "HTTP/%*s %d", &status) != 1) return -1;
	http_last_status = status;

	const char *clp = strstr(hdr, "Content-Length:");
	if (clp) *cl = atol(clp + 15);

	const char *ctp = strstr(hdr, "Content-Type:");
	if (ctp && strstr(ctp, "text/html")) http_last_html = 1;

	return (status >= 200 && status < 300) ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/* body -> memory                                                     */
/* ------------------------------------------------------------------ */
static long stream_mem(int s, unsigned char *buf, long max, long content_length,
                       http_progress_t cb) {
	long done = 0;
	int c;
	while ((c = bgetc(s)) >= 0) {
		if (done >= max) break;
		buf[done++] = (unsigned char)c;
		if (content_length >= 0 && done >= content_length) break;
		if (cb && (done & 0x1fff) == 0) cb(done, content_length);
	}
	if (content_length >= 0 && done < content_length) return -1;
	return done;
}

/* ------------------------------------------------------------------ */
/* body -> file                                                       */
/* ------------------------------------------------------------------ */
static int stream_file(int s, FILE *f, long content_length,
                       http_progress_t cb) {
	char chunk[1024];
	int ci = 0;
	long done = 0;
	int c;

	while (1) {
		c = bgetc(s);
		if (c < 0) break;
		chunk[ci++] = (char)c;
		if (ci == 1024) {
			if (fwrite(chunk, 1, ci, f) != (size_t)ci)
				return -1;
			done += ci;
			ci = 0;
		}
		if (content_length >= 0 && done + ci >= content_length) {
			if (ci) {
				if (fwrite(chunk, 1, ci, f) != (size_t)ci)
					return -1;
				done += ci;
				ci = 0;
			}
			break;
		}
		if (cb && (done & 0x1fff) < 1024)
			cb(done, content_length);
	}
	if (ci) {
		if (fwrite(chunk, 1, ci, f) != (size_t)ci) return -1;
		done += ci;
	}
	if (content_length >= 0 && done < content_length) return -1;
	return 0;
}

/* ------------------------------------------------------------------ */
long http_get_to_mem(const char *path, unsigned char *buf, long max,
                     http_progress_t progress) {
	int s = connect_http();
	if (s < 0) return -1;
	if (send_request(s, path) != 0) { closesocket(s); return -1; }
	long cl = -1;
	if (read_headers(s, &cl) != 0) { closesocket(s); return -1; }
	if (http_last_html) { closesocket(s); return -1; }
	long got = stream_mem(s, buf, max, cl, progress);
	shutdown(s, 0);
	closesocket(s);
	return got;
}

int http_get_to_file(const char *path, const char *fatfile,
                     http_progress_t progress) {
	int s = connect_http();
	if (s < 0) return -1;
	if (send_request(s, path) != 0) { closesocket(s); return -1; }
	long cl = -1;
	if (read_headers(s, &cl) != 0) { closesocket(s); return -1; }
	if (http_last_html) { closesocket(s); return -1; }

	FILE *f = fopen(fatfile, "wb");
	if (!f) { closesocket(s); return -1; }
	int rc = stream_file(s, f, cl, progress);
	fclose(f);
	shutdown(s, 0);
	closesocket(s);
	if (rc != 0) {
		remove(fatfile);
		return -1;
	}
	return 0;
}