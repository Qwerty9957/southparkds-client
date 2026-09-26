#ifndef INDEX_H
#define INDEX_H

typedef struct {
	int num;    /* season number */
	int count;  /* number of episodes in this season */
	int *eps;   /* episode numbers */
} Season;

typedef struct {
	int count;
	Season *seasons;
} Index;

/* Parse the episode index JSON (see README for format). Returns 0 ok, -1 bad. */
int index_parse(const char *json, Index *ix);

/* Free memory owned by ix. */
void index_free(Index *ix);

#endif