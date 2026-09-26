#ifndef CONFIG_H
#define CONFIG_H

/*
 * SouthparkDS configuration.
 *
 * Everything is fetched over PLAIN HTTP on port 80 - the DSi homebrew
 * socket stack (dswifi) has no TLS support, which is why nothing here may
 * use https.
 *
 * Point these values at a plain-HTTP source you control. The expected
 * layout on the server is:
 *
 *   <BASE>index.json          episode index (see README for format)
 *   <BASE>1_1.fv ...          FastVideoDS episode files
 *                             named <season>_<episode>.fv
 */

/* Host the files are served from (no scheme, no port). */
#define HTTP_HOST   "10.0.0.39"

/* Port the server listens on (server port in config.json). */
#define HTTP_PORT   8080

/* URL prefix on that host. Must start with '/'. */
#define HTTP_PREFIX "/"

/* Name of the episode index file, relative to HTTP_PREFIX. */
#define INDEX_FILE  "index.json"

/*
 * SD card directories. The filesystem root that works depends on how the
 * app is launched; we try these in order at startup.
 */
#define CACHE_DIR   "videos"

#define APP_TITLE   "SouthparkDS"

#endif