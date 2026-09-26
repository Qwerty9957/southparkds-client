#include <stdlib.h>
#include <string.h>
#include "index.h"

static void skipws(const char **p) {
	while (**p == ' ' || **p == '\t' || **p == '\n' || **p == '\r')
		(*p)++;
}

int index_parse(const char *json, Index *ix) {
	const char *p = json;
	Season *cur = NULL;

	if (ix) {
		ix->count = 0;
		ix->seasons = NULL;
	}

	while (p && *p) {
		char c = *p;

		if (c == '{' || c == '}' || c == '[' || c == ']' || c == ':' || c == ',') {
			p++;
			continue;
		}

		if (c == '"') {
			char key[24];
			size_t k = 0;
			p++;
			while (*p && *p != '"') {
				if (k < sizeof(key) - 1) key[k++] = *p;
				p++;
			}
			if (*p == '"') p++;
			key[k] = 0;

			skipws(&p);
			if (*p == ':') p++;
			skipws(&p);

			if (!strcmp(key, "season")) {
				long num = strtol(p, (char **)&p, 10);
				Season *ns = realloc(ix->seasons,
					(ix->count + 1) * sizeof(Season));
				if (!ns) return -1;
				ix->seasons = ns;
				cur = &ix->seasons[ix->count];
				cur->num = (int)num;
				cur->count = 0;
				cur->eps = NULL;
				ix->count++;
			} else if (!strcmp(key, "episodes") && cur) {
				skipws(&p);
				if (*p == '[') p++;
				skipws(&p);
				while (*p && *p != ']') {
					if ((*p >= '0' && *p <= '9') || *p == '-') {
						long e = strtol(p, (char **)&p, 10);
						int *ne = realloc(cur->eps,
							(cur->count + 1) * sizeof(int));
						if (!ne) return -1;
						cur->eps = ne;
						cur->eps[cur->count++] = (int)e;
					} else {
						p++;
					}
				}
				if (*p == ']') p++;
			}
			continue;
		}

		p++;
	}

	return (ix && ix->count > 0) ? 0 : -1;
}

void index_free(Index *ix) {
	int i;
	if (!ix) return;
	for (i = 0; i < ix->count; i++)
		free(ix->seasons[i].eps);
	free(ix->seasons);
	ix->seasons = NULL;
	ix->count = 0;
}