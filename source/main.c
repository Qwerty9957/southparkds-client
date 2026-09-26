#include <nds.h>
#include <dswifi9.h>
#include <fat.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include <sys/stat.h>

#include "config.h"
#include "http.h"
#include "index.h"
#include "settings.h"

/* ------------------------------------------------------------------ */
static PrintConsole console_top, console_bottom;

/* Order matters: on a DSi under TWiLight the SD card is "sd:" (the
 * "fat:" device may map to an absent/leftover flashcard driver whose
 * mkdir() never returns). Try sd: first, fat: as fallback. */
static const char *roots[] = { "sd:", "fat:" };
static const char *sp_root = "fat:";

static Index g_index;
static char g_dlname[48];      /* filename currently being downloaded */

/* Power management: calico needs pmMainLoop() called every frame so it can
 * process the lid-close event. Closing the DS then power-off both screens
 * (hardware sleep) and the app resumes where it left off when reopened --
 * no screen burn-in, app never shut down. Returns 1 to keep running. */
static int g_pm_exit;          /* set when calico says the app should exit */
static int pm_step(void) {
	if (!pmMainLoop()) {
		g_pm_exit = 1;
		return 0;
	}
	return 1;
}

/* Sleep handling.
 * calico's ARM7 hinge auto-sleep is unreliable under this loader on the DSi:
 * the ARM7's svcSleep() is woken instantly by the hinge IRQ while the lid
 * stays closed, and its hinge counter never re-arms, so the LCDs never stay
 * dark. ARM9-driven blanking DOES work on the physical DSi (pmEnterSleep turns
 * the LCDs off via REG_POWCNT / POWCNT_LCD), so we drive the screens
 * ourselves: debounce the lid-close for ~30 frames, blank both screens, and
 * poll the hinge until it reopens. pmSetSleepAllowed(false) while blanked
 * stops calico's pmMainLoop() from racing us with its own sleep order. */
static int g_lid_frames;
static void pm_lid_check(void) {
	if (keysHeld() & KEY_HINGE) {
		if (++g_lid_frames == 1)
			pmSetSleepAllowed(false);   /* don't let calico race us */

		if (g_lid_frames < 30)
			return;                     /* ~0.5 s debounce before blanking */

		u32 powcnt = REG_POWCNT;
		REG_POWCNT = powcnt &~ POWCNT_LCD;   /* blank both screens */
		while (keysHeld() & KEY_HINGE) {
			if (!pm_step()) break;
			for (volatile u32 i = 0; i < 400000; i++);  /* ~15-20 ms poll */
			scanKeys();
		}
		REG_POWCNT = powcnt;                /* screens back on */
	}
	pmSetSleepAllowed(true);
	g_lid_frames = 0;
}

/* UI state */
enum {
	VC_SEASONS = 0,
	VC_EPISODES,
	VC_CONFIRM,
	VC_MSG,
	VC_KEYBOARD,
	VC_BOOTERR
};
static int vc = VC_SEASONS;
static int sel = 0;          /* selected row */
static int top_line = 0;     /* first visible row (scrolling) */
static int cur_season = -1;  /* index into g_index.seasons for episodes view */

#define MAX_ITEMS 400
static char  g_items[MAX_ITEMS][40];
static int   g_kind[MAX_ITEMS]; /* 0 season, 1 episode, 2 "..", 3 clear-cache,
                                   4 server url, 5 test conn, 6 get player */
static int   g_s[MAX_ITEMS];
static int   g_e[MAX_ITEMS];
static int g_item_count;

static char g_msg[400];

static Settings g_settings;

/* forward decls */
static int boot_to_index(void);
static void dbg_clr(const char *s);

/* on-screen keyboard state */
static char kb_buf[256];
static int kb_pos;
static int kb_row, kb_col;
static int kb_from_boot;             /* editing URL from the boot-error screen */
static const char *kb_rows[] = {
	"abcdefghijklm",
	"nopqrstuvwxyz",
	"0123456789",
	".:/-_?=&",
	" ",
};
#define KB_ROWS ((int)(sizeof kb_rows / sizeof kb_rows[0]))

/* ------------------------------------------------------------------ */
static void video_path(char *out, size_t cap, int s, int e) {
	snprintf(out, cap, "%s/%d_%d.fv", sp_root, s, e);
}

static void current_path(char *out, size_t cap) {
	snprintf(out, cap, "%s/current.fv", sp_root);
}

static void sp_dir_path(char *out, size_t cap) {
	snprintf(out, cap, "%s/SouthPark", sp_root);
}

static int file_exists(const char *path) {
	FILE *f = fopen(path, "rb");
	if (f) { fclose(f); return 1; }
	return 0;
}

static int dir_exists(const char *path) {
	return FAT_getAttr(path) >= 0;
}

static int copy_file(const char *dst, const char *src) {
	FILE *in = fopen(src, "rb");
	FILE *out;
	char buf[4096];
	size_t n;

	if (!in) return -1;
	out = fopen(dst, "wb");
	if (!out) { fclose(in); return -1; }
	while ((n = fread(buf, 1, sizeof buf, in)) > 0) {
		if (fwrite(buf, 1, n, out) != n) {
			fclose(out); fclose(in); remove(dst);
			return -1;
		}
	}
	fclose(out);
	fclose(in);
	return 0;
}

static int ensure_dirs(void) {
	char d1[64], d2[64];
	int i;
	for (i = 0; i < 2; i++) {
		snprintf(d1, sizeof d1, "%s/SouthPark", roots[i]);
		snprintf(d2, sizeof d2, "%s/SouthPark/videos", roots[i]);
		if (!dir_exists(d1)) {
			dbg_clr("s2 mkdir1%"); {
				int r1 = mkdir(d1, 0755);
				dbg_clr("s2 mk1 ret");
				if (r1 != 0 && errno != EEXIST) continue;
			}
		}
		if (!dir_exists(d2)) {
			dbg_clr("s2 mkdir2%"); {
				int r2 = mkdir(d2, 0755);
				dbg_clr("s2 mk2 ret");
				if (r2 != 0 && errno != EEXIST) continue;
			}
		}
		dbg_clr("s2 root ok");
		sp_root = roots[i];
		return 0;
	}
	dbg_clr("s2 dirs FAIL");
	return -1;
}

/* ------------------------------------------------------------------ */
static const char *assoc_name(int st) {
	switch (st) {
	case ASSOCSTATUS_DISCONNECTED:   return "DISCONNECTED";
	case ASSOCSTATUS_SEARCHING:      return "SEARCHING";
	case ASSOCSTATUS_ASSOCIATING:    return "CONNECTING";
	case ASSOCSTATUS_ACQUIRINGDHCP:  return "GETTING IP";
	case ASSOCSTATUS_ASSOCIATED:     return "CONNECTED";
	default:                         return "?";
	}
}

static int connect_wifi(void) {
	consoleSelect(&console_top);
	consoleClear();
	printf("%s\n", APP_TITLE);
	printf("\nConnecting to WiFi...\n");
	printf("Press START to abort.\n\n");

	/* Loader/previous-app state can leave the radio in a bad state
	 * (esp. when launched via TWiLight). Try a few times. */
	if (!Wifi_InitDefault(WFC_CONNECT)) {
		printf("  (init glitch, retrying)\n");
		swiWaitForVBlank();
		Wifi_InitDefault(WFC_CONNECT);
	}

	for (int attempt = 1; attempt <= 3; attempt++) {
		int frames = 0;
		while (Wifi_AssocStatus() != ASSOCSTATUS_ASSOCIATED) {
			if (!pm_step()) return 1;
			swiWaitForVBlank();
			scanKeys();
			pm_lid_check();
			if (keysDown() & KEY_START) return -1;
			if (++frames > 1200) break;           /* ~20 s per attempt */
			if ((frames % 30) == 0) {
				consoleSelect(&console_top);
				printf("  (%s)\n", assoc_name(Wifi_AssocStatus()));
			}
		}
		if (Wifi_AssocStatus() == ASSOCSTATUS_ASSOCIATED) return 0;

		if (attempt < 3) {
			consoleSelect(&console_top);
			printf("  retry %d...\n", attempt);
			Wifi_DisconnectAP();
			for (int w = 0; w < 30; w++) {        /* let the radio settle */
				if (!pm_step()) return 1;
				swiWaitForVBlank();
				scanKeys();
				pm_lid_check();
			}
			Wifi_AutoConnect();
		}
	}
	return -1;
}

static int fetch_index(void) {
	char path[256];
	unsigned char buf[32768];
	snprintf(path, sizeof path, "%s%s", HTTP_PREFIX, INDEX_FILE);

	consoleSelect(&console_top);
	consoleClear();
	printf("%s\n", APP_TITLE);
	printf("\nFetching %s%s ...\n", HTTP_PREFIX, INDEX_FILE);

	long n = http_get_to_mem(path, buf, sizeof buf - 1, NULL);
	if (n <= 0) return -1;
	buf[n] = 0;

	index_free(&g_index);
	return index_parse((char *)buf, &g_index);
}

/* ------------------------------------------------------------------ */
/* returns 0 on a healthy /health answer, -1 otherwise.               */
static int test_connection(void) {
	char path[64];
	unsigned char b[64];
	snprintf(path, sizeof path, "%s%s", HTTP_PREFIX, "health");
	long n = http_get_to_mem(path, b, sizeof b - 1, NULL);
	if (n <= 0) return -1;
	b[n] = 0;
	return strstr((char *)b, "ok") ? 0 : -1;
}

/* ------------------------------------------------------------------ */
static void build_season_items(void) {
	int i, n = 0;
	for (i = 0; i < g_index.count; i++) {
		snprintf(g_items[n], 40, "Season %d", g_index.seasons[i].num);
		g_kind[n] = 0;
		g_s[n] = i;
		g_e[n] = 0;
		n++;
	}
	if (n < MAX_ITEMS) {
		snprintf(g_items[n], 40, "Clear cache...");
		g_kind[n] = 3;
		n++;
	}
	if (n < MAX_ITEMS) {
		snprintf(g_items[n], 40, "Server URL...");
		g_kind[n] = 4;
		n++;
	}
	if (n < MAX_ITEMS) {
		snprintf(g_items[n], 40, "Test connection");
		g_kind[n] = 5;
		n++;
	}
	if (n < MAX_ITEMS) {
		snprintf(g_items[n], 40, "Get FastVideoDS player");
		g_kind[n] = 6;
		n++;
	}
	g_item_count = n;
}

static void build_episode_items(int si) {
	int i, n = 0;
	Season *s = &g_index.seasons[si];

	snprintf(g_items[n], 40, "..");
	g_kind[n] = 2;
	n++;

	for (i = 0; i < s->count && n < MAX_ITEMS; i++) {
		snprintf(g_items[n], 40, "S%d E%d",
		         s->num, s->eps[i]);
		g_kind[n] = 1;
		g_s[n] = s->num;
		g_e[n] = s->eps[i];
		n++;
	}
	g_item_count = n;
}

static void draw_top_header(void) {
	consoleSelect(&console_top);
	iprintf("%s\n", APP_TITLE);
	iprintf("--------------------------------\n");
}

static void draw_list(void) {
	char buf[64];
	int i, vis = 23;

	consoleSelect(&console_bottom);
	consoleClear();

	if (sel < top_line) top_line = sel;
	if (sel >= top_line + vis) top_line = sel - vis + 1;

	for (i = 0; i < g_item_count; i++) {
		if (i < top_line) continue;
		if (i >= top_line + vis) break;
		snprintf(buf, sizeof buf, "%c%.39s",
			(i == sel) ? '>' : ' ', g_items[i]);
		iprintf("%s\n", buf);
	}
	/* No leading \n here: the bottom console is 24 rows and 23 items +
	 * this footer already fill it exactly. A blank line first would
	 * overflow by one, scroll the console up, and hide the top row
	 * (e.g. "Season 1"). */
	iprintf("A=select  B=back  START=quit");
}

static void draw_kb(void) {
	int i;

	consoleSelect(&console_top);
	consoleClear();
	draw_top_header();
	iprintf("SERVER URL\n\n%s\n", kb_buf);
	iprintf("\nPlain http only - type the\n%s:%u\ncurrently (start-tunnel.bat\nprints the ngrok URL).",
	        http_get_host(), http_get_port());

	consoleSelect(&console_bottom);
	consoleClear();
	for (i = 0; i < KB_ROWS; i++) {
		const char *s = kb_rows[i];
		iprintf(i == kb_row ? ">%s\n" : " %s\n", s);
	}
	iprintf(" %*s^\n", kb_row >= 0 ? kb_col + 1 : 0, "");
	iprintf("\nA=type B=del X=clear\nL/R=row  <-/->=cursor\nSTART=save  SELECT=cancel");
}

static void draw_view(void) {
	consoleSelect(&console_top);
	consoleClear();

	switch (vc) {
	case VC_SEASONS:
	case VC_EPISODES:
		draw_top_header();
		if (vc == VC_EPISODES && cur_season >= 0) {
			iprintf("Season %d - select an episode\n",
			        g_index.seasons[cur_season].num);
		} else {
			iprintf("Select a season\n");
		}
		draw_list();
		break;

	case VC_CONFIRM:
		consoleSelect(&console_top);
		iprintf("CLEAR CACHE\n\n");
		iprintf("This deletes every saved .fv\n");
		iprintf("file from the SD card.\n\n");
		iprintf("A=delete   B=cancel\n");
		consoleSelect(&console_bottom);
		consoleClear();
		break;

	case VC_MSG:
		draw_top_header();
		iprintf("%s\n", g_msg);
		iprintf("\nA=ok\n");
		consoleSelect(&console_bottom);
		consoleClear();
		break;

	case VC_KEYBOARD:
		draw_kb();
		break;

	case VC_BOOTERR:
		draw_top_header();
		iprintf("%s\n", g_msg);
		consoleSelect(&console_bottom);
		consoleClear();
		iprintf("\nA=change URL  B=retry\nSTART=quit");
		break;
	}
}

/* ------------------------------------------------------------------ */
static void set_msg(const char *fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(g_msg, sizeof g_msg, fmt, ap);
	va_end(ap);
}

/* fixed rows on the top console for the download status / progress bar */
#define DL_STATUS_ROW 4
#define DL_BAR_ROW    5

/* redraw one fixed-width 32-char line of the top screen at (0, y), so we
 * can update status without clearing/scroll or dot-spam. Uses the VT
 * escape "\x1b[y;xH" supported by the libnds console driver. */
static void line_at(int y, const char *text) {
	char buf[64];
	snprintf(buf, sizeof buf, "\x1b[%d;0H%-32.32s", y, text ? text : "");
	consoleSelect(&console_top);
	iprintf("%s", buf);
}

/* pull a quoted string value from a tiny JSON response, e.g.
 * {"state":"downloading","message":""}. Skips to the value after the
 * first ':' so we get the value, not the key name. */
static int json_str(const char *json, const char *key,
                    char *out, size_t cap) {
	char needle[32];
	const char *p, *q;
	size_t n;

	snprintf(needle, sizeof needle, "\"%s\"", key);
	p = strstr(json, needle);
	if (!p) return -1;
	p = strchr(p, ':');
	if (!p) return -1;
	p = strchr(p, '"');
	if (!p) return -1;
	q = strchr(p + 1, '"');
	if (!q || q <= p + 1) return -1;
	n = (size_t)(q - p - 1);
	if (n >= cap) n = cap - 1;
	memcpy(out, p + 1, n);
	out[n] = 0;
	return 0;
}

static void on_progress(long done, long total) {
	static long last = -1;
	char buf[64], bar[25];
	int cells = 24, filled, i, pct;
	long tick = 262144;

	/* let calico handle lid-close / power-button events during the transfer */
	pmMainLoop();

	/* throttle to ~1 draw / 256 KB, but always draw the final state */
	if (done < total && done - last < tick) return;
	last = done;

	/* NDS long is 32-bit: "100 * done" overflows past ~21 MB and wraps
	 * negative, so divide first instead of scaling first. */
	if (total > 0 && done >= total)
		pct = 100;
	else if (total > 100)
		pct = (int)(done / (total / 100));
	else if (total > 0)
		pct = (int)((100 * done) / total);   /* tiny responses only */
	else
		pct = -1;

	filled = pct >= 0 ? (pct * cells) / 100 : 0;
	for (i = 0; i < cells; i++)
		bar[i] = (i < filled) ? '#' : '.';
	bar[cells] = 0;
	if (pct >= 0)
		snprintf(buf, sizeof buf, "%3d%% %s", pct, bar);
	else
		snprintf(buf, sizeof buf, "%ld KB %s", done >> 10, bar);
	line_at(DL_BAR_ROW, buf);
}

/* ------------------------------------------------------------------ */
static void kb_open(void) {
	if (g_settings.port == 80)
		snprintf(kb_buf, sizeof kb_buf, "%s", g_settings.host);
	else
		snprintf(kb_buf, sizeof kb_buf, "%s:%u",
		         g_settings.host, (unsigned)g_settings.port);
	kb_pos = strlen(kb_buf);
	kb_row = 0;
	kb_col = 0;
	vc = VC_KEYBOARD;
	draw_kb();
}

static void kb_clamp_col(void) {
	int len = (int)strlen(kb_rows[kb_row]);
	if (kb_col >= len) kb_col = len - 1;
	if (kb_col < 0) kb_col = 0;
}

static void kb_insert(void) {
	size_t len = strlen(kb_buf);
	const char *row = kb_rows[kb_row];
	if (len >= sizeof kb_buf - 1) return;
	memmove(kb_buf + kb_pos + 1, kb_buf + kb_pos, len - kb_pos + 1);
	kb_buf[kb_pos] = row[kb_col];
	kb_pos++;
}

static void kb_backspace(void) {
	if (kb_pos <= 0) return;
	memmove(kb_buf + kb_pos - 1, kb_buf + kb_pos,
	        strlen(kb_buf) - kb_pos + 1);
	kb_pos--;
}

static void kb_accept(void) {
	settings_parse(&g_settings, kb_buf);
	http_set_server(g_settings.host, g_settings.port);
	{
		char dir[96];
		sp_dir_path(dir, sizeof dir);
		settings_save(&g_settings, dir);
	}
	if (kb_from_boot) {
		kb_from_boot = 0;
		boot_to_index();
		return;
	}
	build_season_items();
	sel = 0;
	top_line = 0;
	vc = VC_SEASONS;
	draw_view();
}

static void kb_cancel(void) {
	if (kb_from_boot) {
		kb_from_boot = 0;
		vc = VC_BOOTERR;
		draw_view();
		return;
	}
	build_season_items();
	sel = 0;
	top_line = 0;
	vc = VC_SEASONS;
	draw_view();
}

/* keyboard input handler; returns 1 if handled (else 'down' is unused) */
static void handle_keyboard(u32 down) {
	if (down & KEY_UP) {
		kb_row = (kb_row + KB_ROWS - 1) % KB_ROWS;
		kb_clamp_col();
		draw_kb();
	} else if (down & KEY_DOWN) {
		kb_row = (kb_row + 1) % KB_ROWS;
		kb_clamp_col();
		draw_kb();
	} else if (down & KEY_LEFT) {
		if (kb_pos > 0) { kb_pos--; draw_kb(); }
	} else if (down & KEY_RIGHT) {
		if (kb_pos < (int)strlen(kb_buf)) { kb_pos++; draw_kb(); }
	} else if (down & KEY_A) {
		kb_insert();
		draw_kb();
	} else if (down & KEY_B) {
		kb_backspace();
		draw_kb();
	} else if (down & KEY_X) {
		kb_buf[0] = 0;
		kb_pos = 0;
		draw_kb();
	} else if (down & KEY_START) {
		kb_accept();
	} else if (down & KEY_SELECT) {
		kb_cancel();
	}
}

/* ------------------------------------------------------------------ */
static void do_download(int sv, int ev) {
	char path[320], url[320], cur[320];
	char sjson[600], state[32];
	int rc, f, fails = 0;
	struct stat st;

	snprintf(url, sizeof url, "%s%d_%d.json", HTTP_PREFIX, sv, ev);
	video_path(path, sizeof path, sv, ev);

	snprintf(g_dlname, sizeof g_dlname, "%d_%d.fv", sv, ev);
	consoleSelect(&console_top);
	consoleClear();
	draw_top_header();
	iprintf("S%d E%d\n%s:%u\n", sv, ev, http_get_host(),
	        (unsigned)http_get_port());
	line_at(DL_STATUS_ROW, "Contacting server...");

	/* stage 1: poll GET /S_E.json until the server says ready.
	 * The reply can normally be fetched while the server is building,
	 * so the DSi shows each stage ("downloading" -> "converting").
	 * B or START aborts while polling. */
	for (;;) {
		long n = http_get_to_mem(url, (unsigned char *)sjson,
		                         sizeof sjson - 1, NULL);
		state[0] = 0;
		if (n > 0) {
			sjson[n] = 0;
			json_str(sjson, "state", state, sizeof state);
			fails = 0;
		} else {
			fails++;
		}

		if (state[0] && !strcmp(state, "ready"))
			break;
		if (state[0] && !strcmp(state, "error")) {
			set_msg("Server could not build\n%s.\n\nCheck the server\n"
			        "console for details.", g_dlname);
			vc = VC_MSG;
			draw_view();
			return;
		}
		if (fails >= 5) {
			set_msg("Lost contact with the\nserver while waiting\n"
			        "(HTTP %d).\n\nTry again later.", http_last_status);
			vc = VC_MSG;
			draw_view();
			return;
		}

		if (state[0] && !strcmp(state, "downloading"))
			line_at(DL_STATUS_ROW, "Server: downloading episode");
		else if (state[0] && !strcmp(state, "converting"))
			line_at(DL_STATUS_ROW, "Server: converting to .fv");
		else
			line_at(DL_STATUS_ROW, "Server: queued / starting...");

		for (f = 0; f < 90; f++) {   /* ~1.5 s between poll attempts */
			swiWaitForVBlank();
			if (!pm_step()) return;
			scanKeys();
			pm_lid_check();
			if (keysHeld() & (KEY_B | KEY_START)) {
				set_msg("Download cancelled.");
				vc = VC_MSG;
				draw_view();
				return;
			}
		}
	}

	/* stage 2: pull the .fv itself (now cached/streaming on the server) */
	snprintf(url, sizeof url, "%s%d_%d.fv", HTTP_PREFIX, sv, ev);
	line_at(DL_STATUS_ROW, "Uploading from server...");
	line_at(DL_BAR_ROW, "");

	rc = http_get_to_file(url, path, on_progress);
	if (rc == 0 && stat(path, &st) == 0) {
		int cur_ok;
		current_path(cur, sizeof cur);
		cur_ok = copy_file(cur, path);
		set_msg("Saved to:\n%s\n(~%ld KB)\n\nReady. Open this file\n"
		        "from the TWiLight Menu++\nto play it with the\n"
		        "FastVideoDS player.",
		        path, st.st_size / 1024);
		if (cur_ok != 0)
			set_msg("Saved to:\n%s\n(~%ld KB)\n\nBut copying to\ncurrent.fv FAILED.",
			        path, st.st_size / 1024);
	} else if (rc == 0) {
		set_msg("Saved to:\n%s", path);
	} else {
		g_dlname[0] = 0;
		if (http_last_html) {
			set_msg("Got an HTML page instead\n"
			        "of the video (HTTP %d).\n"
			        "Is the tunnel URL an\n"
			        "http:// ngrok address and\n"
			        "the header being sent?", http_last_status);
		} else {
			set_msg("Download failed (HTTP %d).\n"
			        "Check the server / WiFi,\n"
			        "then try again.", http_last_status);
		}
	}
}

static void do_clear_cache(void) {
	char path[320];
	int i, e, removed = 0;

	for (i = 0; i < g_index.count; i++) {
		for (e = 0; e < g_index.seasons[i].count; e++) {
			video_path(path, sizeof path,
				g_index.seasons[i].num,
				g_index.seasons[i].eps[e]);
			if (file_exists(path)) {
				if (remove(path) == 0) removed++;
			}
		}
	}
	set_msg("Cache cleared: %d file(s) deleted.", removed);
}

static void do_get_player(void) {
	char url[320], path[320];
	int rc;

	snprintf(url, sizeof url, "%s%s", HTTP_PREFIX, "FastVideoDS.nds");
	snprintf(path, sizeof path, "%s/SouthPark/FastVideoDS.nds", sp_root);

	consoleSelect(&console_top);
	consoleClear();
	draw_top_header();
	iprintf("Downloading the FastVideoDS\nplayer to\n%s\n\n", path);

	rc = http_get_to_file(url, path, on_progress);
	if (rc == 0) {
		set_msg("Player saved:\n%s\n\nStart it once so it\n"
		        "shows up in TWiLight\nMenu++.", path);
	} else {
		set_msg("Player download failed\n(HTTP %d%s).\n"
		        "Is the server (and tunnel)\n"
		        "reachable?",
		        http_last_status, http_last_html ? ", html page" : "");
	}
}

/* boot: health test then index; -1 -> VC_BOOTERR shown, 0 -> seasons */
static int boot_to_index(void) {
	consoleSelect(&console_top);
	consoleClear();
	draw_top_header();
	iprintf("Testing connection to\n%s:%u ...\n",
	        http_get_host(), (unsigned)http_get_port());

	if (test_connection() != 0) {
		set_msg("Could not reach %s:%u.\n\nIs the PC server running\n"
		        "(start-server.bat)?\nIs the tunnel up\n(start-tunnel.bat) and\n"
		        "its URL set below?",
		        http_get_host(), (unsigned)http_get_port());
		vc = VC_BOOTERR;
		draw_view();
		return -1;
	}

	if (fetch_index() != 0) {
		set_msg("Server up, but index.json\nfailed (HTTP %d%s).\n\n"
		        "The tunnel may be answering\nwith a warning page instead\n"
		        "of the file.",
		        http_last_status, http_last_html ? ", html page" : "");
		vc = VC_BOOTERR;
		draw_view();
		return -1;
	}

	build_season_items();
	sel = 0;
	top_line = 0;
	vc = VC_SEASONS;
	draw_view();
	return 0;
}

/* ------------------------------------------------------------------ */
static void act(void) {
	switch (g_kind[sel]) {
	case 0: /* season -> episodes */
		cur_season = g_s[sel];
		build_episode_items(cur_season);
		sel = 0;
		top_line = 0;
		vc = VC_EPISODES;
		draw_view();
		break;

	case 1: /* episode -> play / download */
	{
		char path[320], cur[320];
		video_path(path, sizeof path, g_s[sel], g_e[sel]);
		if (file_exists(path)) {
			current_path(cur, sizeof cur);
			if (copy_file(cur, path) == 0)
				set_msg("Ready to play:\n%s\n\nLaunch the FastVideoDS\nplayer to play it.",
				        path);
			else
				set_msg("Already downloaded:\n%s\n\nBut making it the\ncurrent.fv FAILED.",
				        path);
		} else {
			do_download(g_s[sel], g_e[sel]);
		}
		vc = VC_MSG;
		draw_view();
		break;
	}

	case 2: /* ".." */
		build_season_items();
		vc = VC_SEASONS;
		sel = 0;
		top_line = 0;
		draw_view();
		break;

	case 3: /* clear cache */
		vc = VC_CONFIRM;
		draw_view();
		break;

	case 4: /* change server URL */
		kb_open();
		break;

	case 5: /* test connection */
		consoleSelect(&console_top);
		consoleClear();
		draw_top_header();
		iprintf("Testing connection to\n%s:%u ...\n",
		        http_get_host(), (unsigned)http_get_port());
		if (test_connection() == 0) {
			set_msg("Connection OK:\n%s:%u\nanswers /health.",
			        http_get_host(), (unsigned)http_get_port());
		} else {
			set_msg("Connection FAILED to\n%s:%u\n\nCheck the PC server and\n"
			        "the tunnel, or change the\nURL from the menu.",
			        http_get_host(), (unsigned)http_get_port());
		}
		vc = VC_MSG;
		draw_view();
		break;

	case 6: /* get player */
		do_get_player();
		vc = VC_MSG;
		draw_view();
		break;
	}
}

/* ------------------------------------------------------------------ */
/* ------------------------------------------------------------------ */
static int dbg_init(void) {
	consoleSelect(&console_top);
	consoleClear();
	iprintf("S0 console ok\n");
	return 0;
}

static void dbg_clr(const char *s) {
	consoleSelect(&console_top);
	iprintf("S1 %s\n", s);
}

int main(void) {
	int app_on = 1;

	videoSetMode(MODE_0_2D);
	videoSetModeSub(MODE_0_2D);
	vramSetBankA(VRAM_A_MAIN_BG);
	vramSetBankH(VRAM_H_SUB_BG);

	consoleInit(&console_top, 0, BgType_Text4bpp, BgSize_T_256x256,
	            15, 0, true, true);
	consoleInit(&console_bottom, 1, BgType_Text4bpp, BgSize_T_256x256,
	            15, 0, false, true);

	/* Allow calico's hinge auto-sleep (screens off while the lid is closed) */
	pmSetSleepAllowed(true);

	/* DBG */
	dbg_init();
	dbg_clr("s0");

	fatInitDefault();
	dbg_clr("s1 fat ok");

	if (ensure_dirs() != 0) {
		dbg_clr("s2 nodirs");
		consoleSelect(&console_top);
		iprintf("Cannot create directories.\nCan't access the SD card.\n");
		iprintf("\nPress START to quit.\n");
		while (1) {
			if (!pm_step()) return 0;
			swiWaitForVBlank();
			scanKeys();
			pm_lid_check();
			if (keysDown() & KEY_START) break;
		}
		return 0;
	}
	dbg_clr("s2 dirs ok");

	/* runtime server config from the SD card (falls back to config.h) */
	{
		char dir[96];
		sp_dir_path(dir, sizeof dir);
		dbg_clr("s3 settings_load");
		settings_load(&g_settings, dir);
	}
	dbg_clr("s3 loaded");
	http_set_server(g_settings.host, g_settings.port);
	dbg_clr("s4 http set");

	if (connect_wifi() != 0) {
		if (g_pm_exit) return 0;
		consoleSelect(&console_top);
		iprintf("WiFi failed.\nCheck the DSi settings\n(WiFi for Nintendo WFC)\n");
		iprintf("\nPress START to quit.\n");
		while (1) {
			if (!pm_step()) return 0;
			swiWaitForVBlank();
			scanKeys();
			pm_lid_check();
			if (keysDown() & KEY_START) return 0;
		}
	}

	boot_to_index();

	while (app_on) {
		if (!pm_step()) break;
		swiWaitForVBlank();
		scanKeys();
		pm_lid_check();
		u32 down = keysDown();

		if (vc == VC_KEYBOARD) {
			handle_keyboard(down);
			continue;
		}

		if (vc == VC_BOOTERR) {
			if (down & KEY_A) {
				kb_from_boot = 1;
				kb_open();
			} else if (down & KEY_B) {
				boot_to_index();
			}
			continue;
		}

		if (down & KEY_START) {
			app_on = 0;
			continue;
		}

		if (down & KEY_B) {
			if (vc == VC_EPISODES) {
				build_season_items();
				vc = VC_SEASONS;
				sel = 0;
				top_line = 0;
				draw_view();
			} else if (vc == VC_CONFIRM) {
				vc = VC_SEASONS;
				draw_view();
			} else if (vc == VC_MSG) {
				vc = VC_SEASONS;
				draw_view();
			}
		}

		if (down & KEY_A) {
			if (vc == VC_SEASONS || vc == VC_EPISODES) {
				act();
			} else if (vc == VC_CONFIRM) {
				do_clear_cache();
				vc = VC_MSG;
				draw_view();
			} else if (vc == VC_MSG) {
				vc = VC_SEASONS;
				sel = 0;
				top_line = 0;
				draw_view();
			}
		}

		if (vc == VC_SEASONS || vc == VC_EPISODES) {
			if ((down & KEY_UP) && sel > 0) {
				sel--;
				draw_list();
			}
			if ((down & KEY_DOWN) && sel < g_item_count - 1) {
				sel++;
				draw_list();
			}
		}
	}

	return 0;
}