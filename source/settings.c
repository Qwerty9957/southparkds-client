#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "settings.h"

void settings_parse(Settings *s, const char *server) {
	char tmp[160];
	char *p;
	int len;

	strncpy(tmp, server, sizeof tmp - 1);
	tmp[sizeof tmp - 1] = 0;

	/* strip scheme, e.g. http:// */
	p = strstr(tmp, "://");
	if (p) memmove(tmp, p + 3, strlen(p + 3) + 1);

	/* strip anything after the host:port, e.g. /prefix/path */
	p = strchr(tmp, '/');
	if (p) *p = 0;

	len = (int)strlen(tmp);
	while (len > 0 && (tmp[len - 1] == ' ' || tmp[len - 1] == '\t'))
		tmp[--len] = 0;

	strncpy(s->host, tmp, sizeof s->host - 1);
	s->host[sizeof s->host - 1] = 0;
	s->port = 80;

	/* split trailing :port */
	p = strrchr(s->host, ':');
	if (p) {
		const char *rest = p + 1;
		int allnum = (*rest != 0);
		const char *q;
		for (q = rest; *q; q++)
			if (*q < '0' || *q > '9') allnum = 0;
		if (allnum) {
			int v = atoi(rest);
			if (v > 0 && v <= 65535) {
				*p = 0;
				s->port = (unsigned short)v;
			}
		}
	}

	if (!s->host[0]) {
		strncpy(s->host, HTTP_HOST, sizeof s->host - 1);
		s->host[sizeof s->host - 1] = 0;
		s->port = HTTP_PORT;
	}
}

int settings_load(Settings *s, const char *sp_dir) {
	char path[192];
	char line[192];
	FILE *f;

	strncpy(s->host, HTTP_HOST, sizeof s->host - 1);
	s->host[sizeof s->host - 1] = 0;
	s->port = HTTP_PORT;
	s->has_file = 0;

	snprintf(path, sizeof path, "%s/config.txt", sp_dir);
	f = fopen(path, "r");
	if (!f) return -1;

	while (fgets(line, sizeof line, f)) {
		char key[32], val[160];
		if (sscanf(line, " %31[^=]=%159s", key, val) == 2) {
			if (!strcmp(key, "server")) {
				settings_parse(s, val);
				s->has_file = 1;
			}
		}
	}
	fclose(f);
	return 0;
}

int settings_save(const Settings *s, const char *sp_dir) {
	char path[192];
	FILE *f;

	snprintf(path, sizeof path, "%s/config.txt", sp_dir);
	f = fopen(path, "w");
	if (!f) return -1;
	fprintf(f, "server=%s:%u\n", s->host, (unsigned)s->port);
	fclose(f);
	return 0;
}