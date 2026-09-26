#ifndef SETTINGS_H
#define SETTINGS_H

#include "config.h"

/*
 * Runtime settings, loaded from / saved to <SD>/SouthPark/config.txt
 * (SD path root is decided at startup in main.c).
 *
 * File format:
 *   server=host[:port]     e.g.  server=abc123.ngrok-free.app
 *                              or server=10.0.0.39:8080
 * An explicit /prefix/path and an https:// scheme are accepted but
 * stripped - only plain HTTP is possible on the DSi.
 */

typedef struct {
	char host[128];       /* resolved host, no scheme, no port */
	unsigned short port;  /* resolved port (80 if unspecified) */
	int has_file;         /* 1 if a config.txt was read */
} Settings;

/* Load settings from <sp_dir>/config.txt. Returns 0 ok, -1 no file/error.
 * Missing fields fall back to config.h macros. */
int settings_load(Settings *s, const char *sp_dir);

/* Parse a raw "host[:port][/path]" string (scheme tolerated) into s. */
void settings_parse(Settings *s, const char *server);

/* Write settings to <sp_dir>/config.txt. Returns 0 ok, -1 error. */
int settings_save(const Settings *s, const char *sp_dir);

#endif