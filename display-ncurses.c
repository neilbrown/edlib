/*
 * Copyright Neil Brown ©2015-2023 <neil@brown.name>
 * May be distributed under terms of GPLv2 - see file:COPYING
 *
 * ncurses front end for edlib.
 *
 * There is currently only support for a single terminal window
 * which provides a single pane.
 *
 * Rendering operations are:
 *  draw text with attributes at location
 *  erase with attributes in rectangle
 */

#define RECORD_REPLAY

#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE
#endif
#define _XOPEN_SOURCE_EXTENDED
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include <stdlib.h>
#include <fcntl.h>
#include <time.h>
#include <curses.h>
#include <panel.h>
#include <string.h>
#include <locale.h>
#include <ctype.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <netdb.h>

#include <wand/MagickWand.h>
#ifdef __CHECKER__
// enums confuse sparse...
#define MagickBooleanType int
#endif

#include <term.h>

#define PANE_DATA_TYPE struct display_data
#include "core.h"

#ifdef RECORD_REPLAY
#include <unistd.h>
#include <stdio.h>
#include "md5.h"
typedef char hash_t[MD5_DIGEST_SIZE*2+1];
#endif

#ifdef __CHECKER__
#undef NCURSES_OK_ADDR
#define NCURSES_OK_ADDR(p) ((void*)0 != NCURSES_CAST(const void *, (p)))
#endif

struct col_hash;

struct display_data {
	SCREEN			*scr;
	FILE			*scr_file;
	int			is_xterm;
	struct col_hash		*col_hash;
	int			report_position;
	long			last_event;

	bool			did_close;
	bool			suspended;

	struct buf		paste_buf;
	time_t			paste_start;
	char			*paste_latest;
	int			paste_pending;

	struct pids {
		pid_t		pid;
		struct pids	*next;
	}			*pids;

	char			*rs1, *rs2, *rs3, *clear;
	char			attr_buf[1024];
	#ifdef RECORD_REPLAY
	FILE			*log;
	FILE			*input;
	int			input_sleeping;
	/* Sometimes I get duplicate Display lines, but not consistently.
	 * To avoid these, record last, filter repeats.
	 */
	int			last_cx, last_cy;
	char			last_screen[MD5_DIGEST_SIZE*2+1];
	char			next_screen[MD5_DIGEST_SIZE*2+1];
	/* The next event to generate when idle */
	enum { DoNil, DoMouse, DoKey, DoCheck, DoClose} next_event;
	char			event_info[30];
	struct xy		event_pos;

	int			clears; /* counts of Draw:clear events */
	#endif
};
#include "core-pane.h"

static SCREEN *current_screen;
static void ncurses_text(struct pane *p safe, struct pane *display safe,
			 wchar_t ch, int attr, int pair,
			 short x, short y, short cursor);
static PANEL * safe pane_panel(struct pane *p safe, struct pane *home);
DEF_CMD(input_handle);
DEF_CMD(handle_winch);
static struct map *nc_map;
DEF_LOOKUP_CMD(ncurses_handle, nc_map);

static struct display_data *current_dd;
static void set_screen(struct pane *p)
{
	struct display_data *dd;
	extern void *_nc_globals[100];
	int i;
	static int index = -1, offset=0;

	if (!p) {
		if (current_screen && index >= 0)
			_nc_globals[index] = NULL;
		current_screen = NULL;
		return;
	}
	dd = p->data;
	current_dd = dd;
	if (!dd)
		return;
	if (dd->scr == current_screen)
		return;

	if (index == -1) {
		index = -2;
		for (i=0; i<100; i++)
			if (_nc_globals[i] < (void*)stdscr &&
			    _nc_globals[i]+4*(sizeof(void*)) >= (void*)stdscr) {
				/* This is _nc_windowlist */
				index = i;
				offset = ((void*)stdscr) - _nc_globals[i];
			}
	}

	set_term(dd->scr);
	current_screen = dd->scr;
	if (index >= 0) {
		_nc_globals[index] = ((void*)stdscr) - offset;
	}
}

#ifdef RECORD_REPLAY
DEF_CMD(next_evt);

static bool parse_event(struct pane *p safe);
static bool prepare_recrep(struct pane *p safe)
{
	struct display_data *dd = p->data;
	char *name;

	name = getenv("EDLIB_RECORD");
	if (name)
		dd->log = fopen(name, "w");
	name = getenv("EDLIB_REPLAY");
	if (name)
		dd->input = fopen(name, "r");
	if (getenv("EDLIB_PAUSE"))
		sleep(atoi(getenv("EDLIB_PAUSE")));
	if (dd->input) {
		parse_event(p);
		return True;
	}
	return False;
}

static void close_recrep(struct pane *p safe)
{
	struct display_data *dd = p->data;

	if (dd->log) {
		fprintf(dd->log, "Close %d\n", dd->clears);
		fclose(dd->log);
	}
}

static void record_key(struct pane *p safe, char *key safe)
{
	struct display_data *dd = p->data;
	char q;

	if (!dd->log)
		return;
	if (!strchr(key, '"'))
		q = '"';
	else if (!strchr(key, '\''))
		q = '\'';
	else if (!strchr(key, '/'))
		q = '/';
	else
		return;
	fprintf(dd->log, "Key %c%s%c\n", q,key,q);
	dd->last_cx = -2; /* Force next Display to be shown */
	fflush(dd->log);
}

static void record_mouse(struct pane *p safe, char *key safe, int x, int y)
{
	struct display_data *dd = p->data;
	char q;
	if (!dd->log)
		return;
	if (!strchr(key, '"'))
		q = '"';
	else if (!strchr(key, '\''))
		q = '\'';
	else if (!strchr(key, '/'))
		q = '/';
	else
		return;
	fprintf(dd->log, "Mouse %c%s%c %d,%d\n", q,key,q, x, y);
	dd->last_cx = -2; /* Force next Display to be shown */
	fflush(dd->log);
}

static void record_screen(struct pane *p safe)
{
	struct display_data *dd = p->data;
	struct md5_state ctx;
	uint16_t buf[CCHARW_MAX+5];
	char out[MD5_DIGEST_SIZE*2+1];
	int r,c;

	if (!dd->log && !(dd->input && dd->next_event == DoCheck))
		return;
	set_screen(p);
	md5_init(&ctx);
	for (r = 0; r < p->h; r++)
		for (c = 0; c < p->w; c++) {
			cchar_t cc;
			wchar_t wc[CCHARW_MAX+2];
			attr_t a;
			short color, fg, bg;
			int l;

			mvwin_wch(stdscr, r, c, &cc);
			getcchar(&cc, wc, &a, &color, NULL);
			pair_content(color, &fg, &bg);
			buf[0] = htole16(fg);
			buf[1] = htole16(bg);
			for (l = 0; l < CCHARW_MAX && wc[l]; l++)
				buf[l+3] = htole16(wc[l]);
			buf[2] = htole16(l);
			LOG("%d,%d %d:%d:%d:%d %lc", c,r,fg,bg,l,wc[0], wc[0] > ' ' ? wc[0] : '?');
			md5_update(&ctx, (uint8_t*)buf,
				   (l+3) * sizeof(uint16_t));
		}
	md5_final_txt(&ctx, out);
	if (strcmp(out, dd->last_screen) == 0 &&
	     p->cx == dd->last_cx && p->cy == dd->last_cy) {
		/* No  change - filter it */
		dd->clears -= 1;
	} else if (dd->log) {
		fprintf(dd->log, "Display %d,%d %s", p->w, p->h, out);
		if (p->cx >= 0)
			fprintf(dd->log, " %d,%d", p->cx, p->cy);
		fprintf(dd->log, "\n");
		fflush(dd->log);
		strcpy(dd->last_screen, out);
		dd->last_cx = p->cx; dd->last_cy = p->cy;
	}
	if (dd->input && dd->input_sleeping) {
		char *delay = getenv("EDLIB_REPLAY_DELAY");
		call_comm("event:free", p, &next_evt);
		if (delay)
			call_comm("event:timer", p, &next_evt, atoi(delay));
		else
			call_comm("event:on-idle", p, &next_evt);
	}
}

static char *copy_quote(char *line safe, char *buf safe)
{
	char q;
	while (*line == ' ')
		line++;
	q = *line++;
	if (q != '"' && q != '\'' && q != '/')
		return NULL;
	while (*line != q && *line)
		*buf++ = *line++;
	if (!*line)
		return NULL;
	*buf = '\0';
	return line+1;
}

static char *get_coord(char *line safe, struct xy *co safe)
{
	long v;
	char *ep;

	while (*line == ' ')
		line ++;
	v = strtol(line, &ep, 10);
	if (!ep || ep == line || *ep != ',')
		return NULL;
	co->x = v;
	line = ep+1;
	v = strtol(line, &ep, 10);
	if (!ep || ep == line || (*ep && *ep != ' ' && *ep != '\n'))
		return NULL;
	co->y = v;
	return ep;
}

static char *get_hash(char *line safe, hash_t hash safe)
{
	int i;
	while (*line == ' ')
		line++;
	for (i = 0; i < MD5_DIGEST_SIZE*2 && *line; i++)
		hash[i] = *line++;
	if (!*line)
		return NULL;
	return line;
}

static bool parse_event(struct pane *p safe)
{
	struct display_data *dd = p->data;
	char line[80];

	line[79] = 0;
	dd->next_event = DoNil;
	if (!dd->input ||
	    fgets(line, sizeof(line)-1, dd->input) == NULL)
		line[0]=0;
	else if (strstarts(line, "Key ")) {
		if (!copy_quote(line+4, dd->event_info))
			return False;
		dd->next_event = DoKey;
	} else if (strstarts(line, "Mouse ")) {
		char *f = copy_quote(line+6, dd->event_info);
		if (!f)
			return False;
		f = get_coord(f, &dd->event_pos);
		if (!f)
			return False;
		dd->next_event = DoMouse;
	} else if (strstarts(line, "Display ")) {
		char *f = get_coord(line+8, &dd->event_pos);
		if (!f)
			return False;
		f = get_hash(f, dd->next_screen);
		dd->next_event = DoCheck;
	} else if (strstarts(line, "Close")) {
		dd->next_event = DoClose;
	}
	LOG("parse %s", line);

	dd->input_sleeping = 1;
	if (dd->next_event != DoCheck) {
		char *delay = getenv("EDLIB_REPLAY_DELAY");
		if (delay)
			call_comm("event:timer", p, &next_evt, atoi(delay));
		else
			call_comm("event:on-idle", p, &next_evt);
	} else
		call_comm("event:timer", p, &next_evt, 10*1000);
	return True;
}

REDEF_CMD(next_evt)
{
	struct pane *p = ci->home;
	struct display_data *dd = p->data;
	int button = 0, type = 0;

	dd->input_sleeping = 0;
	switch(dd->next_event) {
	case DoKey:
		record_key(p, dd->event_info);
		call("Keystroke", p, 0, NULL, dd->event_info);
		break;
	case DoMouse:
		record_mouse(p, dd->event_info, dd->event_pos.x,
			     dd->event_pos.y);
		if (strstr(dd->event_info, ":Press"))
			type = 1;
		else if (strstr(dd->event_info, ":Release"))
			type = 2;
		else if (strstr(dd->event_info, ":Motion"))
			type = 3;
		if (type == 1 || type == 2) {
			char *e = dd->event_info + strlen(dd->event_info) - 1;
			button = atoi(e);
		}
		call("Mouse-event", p, button, NULL, dd->event_info,
		     type, NULL, NULL,
		     dd->event_pos.x, dd->event_pos.y);
		break;
	case DoCheck:
		/* No point checking, just do a diff against new trace log. */
		/* not; (strcmp(dd->next_screen, dd->last_screen) != 0) */
		break;
	case DoClose:
		call("event:deactivate", p);
		pane_close(p);
		return 1;
	case DoNil:
		call_comm("event:read", p, &input_handle, 0);
		call_comm("event:signal", p, &handle_winch, SIGWINCH);
		return 1;
	}
	parse_event(p);
	return 1;
}
#else
static inline bool  prepare_recrep(struct pane *p safe) {return False;}
static inline void record_key(struct pane *p safe, char *key) {}
static inline void record_mouse(struct pane *p safe, char *key safe,
				int x, int y) {}
static inline void record_screen(struct pane *p safe) {}
static inline void close_recrep(struct pane *p safe) {}
#endif

DEF_CB(cnt_disp)
{
	struct call_return *cr = container_of(ci->comm, struct call_return, c);

	cr->i += 1;
	return 1;
}

static void ncurses_end(struct pane *p safe);

DEF_CMD(nc_close_display)
{
	/* If this is only display, then refuse to close this one */
	struct call_return cr;
	char *nc = pane_attr_get(ci->home, "no-close");

	if (nc) {
		call("Message", ci->focus, 0, NULL, nc);
		return 1;
	}

	cr.c = cnt_disp;
	cr.i = 0;
	call_comm("editor:notify:all-displays", ci->focus, &cr.c);
	if (cr.i > 1) {
		/* Need to call ncurses_end() before we send a Notify:Close
		 * notification, else server exits too early
		 */
		ncurses_end(ci->home);
		return Efallthrough;
	} else
		call("Message", ci->focus, 0, NULL,
		     "Cannot close only window.");
	return 1;
}

static int nc_putc(int ch)
{
	if (current_dd)
		fputc(ch, current_dd->scr_file);
	return 1;
}

static char *fnormalize(struct pane *p safe, const char *str) safe
{
	char *ret = strsave(p, str);
	char *cp;

	for (cp = ret ; cp && *cp ; cp++)
		if (!isalnum(*cp) &&
		    !strchr("/_-+=.,@#", *cp))
			/* Don't like this char */
			*cp = '_';
	return ret ?: "_";
}

static void wait_for(struct display_data *dd safe)
{
	struct pids **pp = &dd->pids;

	while (*pp) {
		struct pids *p = *pp;
		if (waitpid(p->pid, NULL, WNOHANG) > 0) {
			*pp = p->next;
			free(p);
		} else
			pp = &p->next;
	}
}

DEF_CB(ns_resume)
{
	struct display_data *dd = ci->home->data;

	if (dd->suspended) {
		dd->suspended = False;
		set_screen(ci->home);
		doupdate();
	}
	return 1;
}

DEF_CMD(nc_external_viewer)
{
	struct pane *p = ci->home;
	struct display_data *dd = p->data;
	char *disp = pane_attr_get(p, "DISPLAY");
	char *disp_auth = pane_attr_get(p, "XAUTHORITY");
	char *remote = pane_attr_get(p, "REMOTE_SESSION");
	char *fqdn = NULL;
	const char *path = ci->str;
	int pid;
	char buf[100];
	int n;
	int fd;

	if (!path)
		return Enoarg;
	if (disp && *disp) {
		struct pids *pds;
		switch (pid = fork()) {
		case -1:
			return Efail;
		case 0: /* Child */
			setenv("DISPLAY", disp, 1);
			if (disp_auth)
				setenv("XAUTHORITY", disp_auth, 1);
			fd = open("/dev/null", O_RDWR);
			if (fd) {
				dup2(fd, 0);
				dup2(fd, 1);
				dup2(fd, 2);
				if (fd > 2)
					close(fd);
			}
			execlp("xdg-open", "xdg-open", path, NULL);
			exit(1);
		default: /* parent */
			pds = malloc(sizeof(*pds));
			pds->pid = pid;
			pds->next = dd->pids;
			dd->pids = pds;
			break;
		}
		wait_for(dd);
		return 1;
	}
	/* handle no-display case */
	if (remote && strcmp(remote, "yes") == 0 &&
	    path[0] == '/' &&
	    gethostname(buf, sizeof(buf)) == 0) {
		struct addrinfo *res;
		const struct addrinfo hints = {
			.ai_flags = AI_CANONNAME,
		};
		if (getaddrinfo(buf, NULL, &hints, &res) == 0 &&
		    res && res->ai_canonname)
			fqdn = strdup(res->ai_canonname);
		freeaddrinfo(res);
	}
	set_screen(p);
	n = 0;
	ioctl(fileno(dd->scr_file), FIONREAD, &n);
	if (n)
		n -= read(fileno(dd->scr_file), buf,
			  n <= (int)sizeof(buf) ? n : (int)sizeof(buf));
	endwin();
	/* stay in raw mode */
	raw();
	noecho();

	/* Endwin doesn't seem to reset properly, at least on xfce-terminal.
	 * So do it manually
	 */
	if (dd->rs1)
		tputs(dd->rs1, 1, nc_putc);
	if (dd->rs2)
		tputs(dd->rs2, 1, nc_putc);
	if (dd->rs3)
		tputs(dd->rs3, 1, nc_putc);
	if (dd->clear)
		tputs(dd->clear, 1, nc_putc);
	fflush(dd->scr_file);

	fprintf(dd->scr_file, "# Consider copy-pasting following\r\n");
	if (fqdn && path[0] == '/') {
		/* File will not be local for the user, so help them copy it. */
		const char *tmp = fnormalize(p, ci->str2 ?: "XXXXXX");
		const char *fname = fnormalize(p, ci->str);

		if (strcmp(fname, ci->str) != 0)
			/* file name had unusuable chars, need to create safe name */
			link(ci->str, fname);
		fprintf(dd->scr_file, "f=`mktemp --tmpdir %s`; scp %s:%s $f ; ",
			tmp, fqdn, fname);
		path = "$f";
	}
	free(fqdn);
	fprintf(dd->scr_file, "xdg-open %s\r\n", path);
	fprintf(dd->scr_file, "# Press Enter to continue\r\n");
	dd->suspended = True;
	call_comm("event:timer", p, &ns_resume, 30*1000);
	return 1;
}

static void ncurses_stop(struct pane *p safe)
{
	struct display_data *dd = p->data;

	if (dd->is_xterm) {
		/* disable bracketed-paste */
		fprintf(dd->scr_file, "\033[?2004l");
		fflush(dd->scr_file);
	}
	if (dd->paste_start)
		free(buf_final(&dd->paste_buf));
	dd->paste_start = 0;
	free(dd->paste_latest);
	dd->paste_latest = NULL;
	nl();
	endwin();
	if (dd->rs1)
		tputs(dd->rs1, 1, nc_putc);
	if (dd->rs2)
		tputs(dd->rs2, 1, nc_putc);
	if (dd->rs3)
		tputs(dd->rs3, 1, nc_putc);
	fflush(dd->scr_file);
}

static void ncurses_end(struct pane *p safe)
{
	struct display_data *dd = p->data;

	if (dd->did_close)
		return;
	dd->did_close = True;
	set_screen(p);
	close_recrep(p);

	ncurses_stop(p);
}

/*
 * hash table for colours and pairs
 * key is r,g,b (0-1000) in 10bit fields,
 * or fg,bg in 16 bit fields with bit 31 set
 * content is colour number of colour pair number.
 * We never delete entries, unless we delete everything.
 */

struct chash {
	struct chash *next;
	int key, content;
};
#define COL_KEY(r,g,b) ((r<<20) | (g<<10) | b)
#define PAIR_KEY(fg, bg) ((1<<31) | (fg<<16) | bg)
#define hash_key(k) ((((k) * 0x61C88647) >> 20) & 0xff)

struct col_hash {
	int next_col, next_pair;
	struct chash *tbl[256];
};

static struct col_hash *safe hash_init(struct display_data *dd safe)
{
	if (!dd->col_hash) {
		dd->col_hash = malloc(sizeof(*dd->col_hash));
		memset(dd->col_hash, 0, sizeof(*dd->col_hash));
		dd->col_hash->next_col = 16;
		dd->col_hash->next_pair = 1;
	}
	return dd->col_hash;
}

static void hash_free(struct display_data *dd safe)
{
	int h;
	struct chash *c;
	struct col_hash *ch;

	ch = dd->col_hash;
	if (!ch)
		return;
	for (h = 0; h < 255; h++)
		while ((c = ch->tbl[h]) != NULL) {
			ch->tbl[h] = c->next;
			free(c);
		}
	free(ch);
	dd->col_hash = NULL;
}

static int find_col(struct display_data *dd safe, int rgb[])
{
	if (0 /* dynamic colours */) {
		struct col_hash *ch = hash_init(dd);
		int k = COL_KEY(rgb[0], rgb[1], rgb[2]);
		int h = hash_key(k);
		struct chash *c;

		for (c = ch->tbl[h]; c; c = c->next)
			if (c->key == k)
				return c->content;
		c = malloc(sizeof(*c));
		c->key = k;
		c->content = ch->next_col++;
		c->next = ch->tbl[h];
		ch->tbl[h] = c;
		init_color(c->content, rgb[0], rgb[1], rgb[2]);
		return c->content;
	} else {
		/* If colour is grey, map to 1 of 26 for 0, 232 to 255, 15
		 * The 24 grey shades have bit values from 8 to 238, so the
		 * gap to white is a little bigger, but that probably doesn't
		 * matter.
		 * Otherwise map to 6x6x6 rgb cube from 16
		 * Actual colours are biased bright, at 0,95,135,175,215,255
		 * with a 95 gap at bottom and 40 elsewhere.
		 * So we divide 5 and 2 half ranges, and merge bottom 2.
		 */
		int c = 0;
		int h;

		//printf("want %d,%d,%d\n", rgb[0], rgb[1], rgb[2]);
		if (abs(rgb[0] - rgb[1]) < 10 &&
		    abs(rgb[1] - rgb[2]) < 10) {
			/* grey - within 1% */
			int v = (rgb[0] + rgb[1] + rgb[2]) / 3;

			/* We divide the space in 24 ranges surrounding
			 * the grey values, and 2 half-ranges near black
			 * and white.  So add half a range - 1000/50 -
			 * then divide by 1000/25 to get a number from 0 to 25.
			 */
			v = (v + 1000/50) / (1000/25);
			if (v == 0)
				return 0; /* black */
			if (v >= 25)
				return 15; /* white */
			//printf(" grey %d\n", v + 231);
			/* grey shades are from 232 to 255 inclusive */
			return v + 231;
		}
		for (h = 0; h < 3; h++) {
			int v = rgb[h];

			v = (v + 1000/12) / (1000/6);
			/* v is from 0 to 6, we want up to 5
			 * with 0 and 1 merged
			 */
			if (v)
				v -= 1;

			c = c * 6 + v;
		}
		//printf(" color %d\n", c + 16);
		return c + 16;
	}
}

static int to_pair(struct display_data *dd safe, int fg, int bg)
{
	struct col_hash *ch = hash_init(dd);
	int k = PAIR_KEY(fg, bg);
	int h = hash_key(k);
	struct chash *c;

	for (c = ch->tbl[h]; c; c = c->next)
		if (c->key == k)
			return c->content;
	c = malloc(sizeof(*c));
	c->key = k;
	c->content = ch->next_pair++;
	c->next = ch->tbl[h];
	ch->tbl[h] = c;
	init_pair(c->content, fg, bg);
	return c->content;
}

static int cvt_attrs(struct pane *p safe, struct pane *home safe,
		     const char *attrs, int *pairp safe)
{
	struct display_data *dd = home->data;
	int attr = 0;
	const char *a, *v;
	char *col = NULL;
	PANEL *pan = NULL;
	int fg = COLOR_BLACK;
	int bg = COLOR_WHITE+8;

	set_screen(home);
	while (p->parent != p &&(pan = pane_panel(p, NULL)) == NULL)
		p = p->parent;
	if (pan) {
		/* Get 'default colours for this pane - set at clear */
		int at = getbkgd(panel_window(pan));
		int pair = PAIR_NUMBER(at);
		short dfg, dbg;
		pair_content(pair, &dfg, &dbg);
		if (dfg >= 0)
			fg = dfg;
		if (dbg >= 0)
			bg = dbg;
	}

	foreach_attr(a, v, attrs, NULL) {
		if (amatch(a, "inverse"))
			attr |= A_STANDOUT;
		else if (amatch(a, "noinverse"))
			attr &= ~A_STANDOUT;
		else if (amatch(a, "bold"))
			attr |= A_BOLD;
		else if (amatch(a, "nobold"))
			attr &= ~A_BOLD;
		else if (amatch(a, "underline"))
			attr |= A_UNDERLINE;
		else if (amatch(a, "nounderline"))
			attr &= ~A_UNDERLINE;
		else if (amatch(a, "fg") && v) {
			struct call_return cr =
				call_ret(all, "colour:map", home,
					 0, NULL, aupdate(&col, v));
			int rgb[3] = {cr.i, cr.i2, cr.x};
			fg = find_col(dd, rgb);
		} else if (amatch(a, "bg") && v) {
			struct call_return cr =
				call_ret(all, "colour:map", home,
					 0, NULL, aupdate(&col, v));
			int rgb[3] = {cr.i, cr.i2, cr.x};
			bg = find_col(dd, rgb);
		}
	}
	free(col);
	if (fg != COLOR_BLACK || bg != COLOR_WHITE+8)
		*pairp = to_pair(dd, fg, bg);
	return attr;
}

static int make_cursor(int attr)
{
	return attr ^ A_UNDERLINE;
}

DEF_CMD(nc_notify_display)
{
	struct display_data *dd = ci->home->data;
	comm_call(ci->comm2, "callback:display", ci->home, dd->last_event);
	return 1;
}

DEF_CMD_CLOSED(nc_close)
{
	struct pane *p = ci->home;
	struct display_data *dd = p->data;
	ncurses_end(p);
	hash_free(dd);
	fclose(dd->scr_file);
	return 1;
}

DEF_CMD(nc_pane_close)
{
	PANEL *pan = NULL;

	set_screen(ci->home);
	while ((pan = panel_above(pan)) != NULL)
		if (panel_userptr(pan) == ci->focus)
			break;
	if (pan) {
		WINDOW *win = panel_window(pan);
		del_panel(pan);
		delwin(win);
		pane_damaged(ci->home, DAMAGED_POSTORDER);
	}
	return 1;
}

static PANEL * safe pane_panel(struct pane *p safe, struct pane *home)
{
	PANEL *pan = NULL;

	while ((pan = panel_above(pan)) != NULL)
		if (panel_userptr(pan) == p)
			return pan;

	if (!home)
		return pan;

	pan = new_panel(newwin(p->h, p->w, 0, 0));
	set_panel_userptr(pan, p);
	pane_add_notify(home, p, "Notify:Close");

	return pan;
}

DEF_CMD(nc_clear)
{
	struct pane *p = ci->home;
	struct display_data *dd = p->data;
	cchar_t cc = {};
	int pair = 0;
	/* default come from parent when clearing pane */
	int attr = cvt_attrs(ci->focus->parent, p, ci->str, &pair);
	PANEL *panel;
	WINDOW *win;
	int w, h;

	set_screen(p);
	panel = pane_panel(ci->focus, p);
	if (!panel)
		return Efail;
	win = panel_window(panel);
	getmaxyx(win, h, w);
	if (h != ci->focus->h || w != ci->focus->w) {
		wresize(win, ci->focus->h, ci->focus->w);
		replace_panel(panel, win);
	}
	cc.attr = attr;
	cc.ext_color = pair;
	cc.chars[0] = ' ';
	wbkgrndset(win, &cc);
	werase(win);
	dd->clears += 1;

	pane_damaged(p, DAMAGED_POSTORDER);
	return 1;
}

DEF_CMD(nc_text_size)
{
	int max_space = ci->num;
	int max_bytes = 0;
	int size = 0;
	const char *str = ci->str;

	if (!str)
		return Enoarg;
	while (str[0] != 0) {
		wint_t wc = get_utf8(&str, NULL);
		int width;
		if (wc >= WERR)
			break;
		width = wcwidth(wc);
		if (width < 0)
			break;
		size += width;
		if (size <= max_space)
			max_bytes = str - ci->str;
	}
	return comm_call(ci->comm2, "callback:size", ci->focus,
			 max_bytes, NULL, NULL,
			 0, NULL, NULL, size, 1);
}

DEF_CMD(nc_draw_text)
{
	struct pane *p = ci->home;
	int pair = 0;
	int attr = cvt_attrs(ci->focus, p, ci->str2, &pair);
	int cursor_offset = ci->num;
	short x = ci->x, y = ci->y;
	const char *str = ci->str;

	if (!str)
		return Enoarg;
	set_screen(p);
	while (str[0] != 0) {
		int precurs = str <= ci->str + cursor_offset;
		wint_t wc = get_utf8(&str, NULL);
		int width;
		if (wc == WEOF || wc == WERR)
			break;
		width = wcwidth(wc);
		if (width < 0)
			break;
		if (precurs && str > ci->str + cursor_offset)
			ncurses_text(ci->focus, p, wc, attr, pair, x, y, 1);
		else
			ncurses_text(ci->focus, p, wc, attr, pair, x, y, 0);
		x += width;
	}
	if (str == ci->str + cursor_offset)
		ncurses_text(ci->focus, p, ' ', 0, 0, x, y, 1);
	pane_damaged(p, DAMAGED_POSTORDER);
	return 1;
}

DEF_CMD(nc_draw_image)
{
	/* 'str' identifies the image. Options are:
	 *     file:filename  - load file from fs
	 *     comm:command   - run command collecting bytes
	 * 'num' is '16' if image should be stretched to fill pane
	 * Otherwise it is the 'or' of
	 *   0,1,2 for left/middle/right in x direction
	 *   0,4,8 for top/middle/bottom in y direction
	 * only one of these can be used as image will fill pane
	 * in other direction.
	 * If 'x' and 'y' are both positive, draw cursor box at
	 * p->cx, p->cy of a size so that 'x' will fit across and
	 * 'y' will fit down.
	 */
	struct pane *p = ci->home;
	struct display_data *dd = p->data;
	int x = 0, y = 0;
	bool stretch = ci->num & 16;
	int pos = ci->num;
	int w = ci->focus->w, h = ci->focus->h * 2;
	int cx = -1, cy = -1;
	MagickBooleanType status;
	MagickWand *wd;
	unsigned char *buf;
	int i, j;

	if (!ci->str)
		return Enoarg;
	if (strstarts(ci->str, "file:")) {
		wd = NewMagickWand();
		status = MagickReadImage(wd, ci->str + 5);
		if (status == MagickFalse) {
			DestroyMagickWand(wd);
			return Efail;
		}
	} else if (strstarts(ci->str, "comm:")) {
		struct call_return cr;
		wd = NewMagickWand();
		cr = call_ret(bytes, ci->str+5, ci->focus);
		if (!cr.s) {
			DestroyMagickWand(wd);
			return Efail;
		}
		status = MagickReadImageBlob(wd, cr.s, cr.i);
		free(cr.s);
		if (status == MagickFalse) {
			DestroyMagickWand(wd);
			return Efail;
		}
	} else
		return Einval;

	MagickAutoOrientImage(wd);
	if (!stretch) {
		int ih = MagickGetImageHeight(wd);
		int iw = MagickGetImageWidth(wd);

		if (iw <= 0 || iw <= 0) {
			DestroyMagickWand(wd);
			return Efail;
		}
		if (iw * h > ih * w) {
			/* Image is wider than space, use less height */
			ih = ih * w / iw;
			switch(pos & (8+4)) {
			case 4: /* center */
				y = (h - ih) / 2; break;
			case 8: /* bottom */
				y = h - ih; break;
			}
			/* Keep 'h' even! */
			h = ((ih+1)/2) * 2;
		} else {
			/* image is too tall, use less width */
			iw = iw * h / ih;
			switch (pos & (1+2)) {
			case 1: /* center */
				x = (w - iw) / 2; break;
			case 2: /* right */
				x = w - iw ; break;
			}
			w = iw;
		}
	}
	MagickAdaptiveResizeImage(wd, w, h);
	buf = malloc(h * w * 4);
	MagickExportImagePixels(wd, 0, 0, w, h, "RGBA", CharPixel, buf);

	if (ci->x > 0 && ci->y > 0 && ci->focus->cx >= 0) {
		/* We want a cursor */
		cx = x + ci->focus->cx;
		cy = y + ci->focus->cy;
	}
	for (i = 0; i < h; i+= 2) {
		static const wint_t hilo = 0x2580; /* L'▀' */
		for (j = 0; j < w ; j+= 1) {
			unsigned char *p1 = buf + i*w*4 + j*4;
			unsigned char *p2 = buf + (i+1)*w*4 + j*4;
			int rgb1[3] = { p1[0]*1000/255, p1[1]*1000/255, p1[2]*1000/255 };
			int rgb2[3] = { p2[0]*1000/255, p2[1]*1000/255, p2[2]*1000/255 };
			int fg = find_col(dd, rgb1);
			int bg = find_col(dd, rgb2);

			if (p1[3] < 128 || p2[3] < 128) {
				/* transparent */
				cchar_t cc;
				short f,b;
				struct pane *pn2 = ci->focus;
				PANEL *pan = pane_panel(pn2, NULL);

				while (!pan && pn2->parent != pn2) {
					pn2 = pn2->parent;
					pan = pane_panel(pn2, NULL);
				}
				if (pan) {
					wgetbkgrnd(panel_window(pan), &cc);
					if (cc.ext_color == 0)
						/* default.  This is light
						 * gray rather then white,
						 * but I think it is a good
						 * result.
						 */
						b = COLOR_WHITE;
					else
						pair_content(cc.ext_color, &f, &b);
					if (p1[3] < 128)
						fg = b;
					if (p2[3] < 128)
						bg = b;
				}
			}
			/* FIXME this doesn't work because
			 * render-line knows too much and gets it wrong.
			 */
			if (cx == x+j && cy == y + (i/2))
				ncurses_text(ci->focus, p, 'X', 0,
					     to_pair(dd, 0, 0),
					     x+j, y+(i/2), 1);
			else
				ncurses_text(ci->focus, p, hilo, 0,
					     to_pair(dd, fg, bg),
					     x+j, y+(i/2), 0);

		}
	}
	free(buf);

	DestroyMagickWand(wd);

	pane_damaged(ci->home, DAMAGED_POSTORDER);

	return 1;
}

DEF_CMD(nc_image_size)
{
	MagickBooleanType status;
	MagickWand *wd;
	int ih, iw;

	if (!ci->str)
		return Enoarg;
	if (strstarts(ci->str, "file:")) {
		wd = NewMagickWand();
		status = MagickReadImage(wd, ci->str + 5);
		if (status == MagickFalse) {
			DestroyMagickWand(wd);
			return Efail;
		}
	} else if (strstarts(ci->str, "comm:")) {
		struct call_return cr;
		wd = NewMagickWand();
		cr = call_ret(bytes, ci->str+5, ci->focus);
		if (!cr.s) {
			DestroyMagickWand(wd);
			return Efail;
		}
		status = MagickReadImageBlob(wd, cr.s, cr.i);
		free(cr.s);
		if (status == MagickFalse) {
			DestroyMagickWand(wd);
			return Efail;
		}
	} else
		return Einval;

	MagickAutoOrientImage(wd);
	ih = MagickGetImageHeight(wd);
	iw = MagickGetImageWidth(wd);

	DestroyMagickWand(wd);
	comm_call(ci->comm2, "callback:size", ci->focus,
		  0, NULL, NULL, 0, NULL, NULL,
		  iw, ih);
	return 1;
}

DEF_CMD(nc_refresh_size)
{
	struct pane *p = ci->home;

	set_screen(p);
	getmaxyx(stdscr, p->h, p->w);
	clearok(curscr, 1);
	return 0;
}

DEF_CMD(nc_refresh_post)
{
	struct pane *p = ci->home;
	struct display_data *dd = p->data;
	struct pane *p1;
	PANEL *pan, *pan2;

	if (dd->suspended)
		return 1;

	set_screen(p);

	/* Need to ensure stacking order and panel y,x position
	 * is correct.  FIXME it would be good if we could skip this
	 * almost always.
	 */
	pan = panel_above(NULL);
	if (!pan)
		return 1;
	p1 = (struct pane*) panel_userptr(pan);
	for (;(pan2 = panel_above(pan)) != NULL; pan = pan2) {
		struct pane *p2 = (struct pane*)panel_userptr(pan2);
		p1 = (struct pane*)panel_userptr(pan);
		if (!p1 || !p2)
			continue;

		if (p1->abs_z < p2->abs_z)
			continue;
		if (p1->abs_z == p2->abs_z &&
		    p1->z <= p2->z)
			continue;
		/* pan needs to be above pan2.  All we can do is move it to
		 * the top. Anything that needs to be above it will eventually
		 * be pushed up too.
		 */
		top_panel(pan);
		/* Now the panel below pan might need to be over pan2 too... */
		pan = panel_below(pan2);
		if (pan)
			pan2 = pan;
	}

	/* As we need to crop pane against their parents, we cannot simply
	 * use update_panels().  Instead we copy each to stdscr and refresh
	 * that.
	 */
	for (pan = NULL; (pan = panel_above(pan)) != NULL; ) {
		WINDOW *win;
		struct xy src, dest, destend;
		int w, h;

		p1 = (void*)panel_userptr(pan);
		if (!p1)
			continue;
		dest = pane_mapxy(p1, p, 0, 0, True);
		destend = pane_mapxy(p1, p, p1->w, p1->h, True);
		src = pane_mapxy(p1, p, 0, 0, False);
		src.x = dest.x - src.x;
		src.y = dest.y - src.y;
		win = panel_window(pan);
		getmaxyx(win, h, w);
		/* guard again accessing beyond boundary of win */
		if (destend.x > dest.x + (w - src.x))
			destend.x = dest.x + (w - src.x);
		if (destend.y > dest.y + (h - src.y))
			destend.y = dest.y - (h - src.y);
		copywin(win, stdscr, src.y, src.x,
			dest.y, dest.x, destend.y-1, destend.x-1, 0);
	}
	/* place the cursor */
	p1 = pane_focus(p);
	pan = NULL;
	while (p1 != p && (pan = pane_panel(p1, NULL)) == NULL)
		p1 = p1->parent;
	if (pan && p1->cx >= 0) {
		struct xy curs = pane_mapxy(p1, p, p1->cx, p1->cy, False);
		wmove(stdscr, curs.y, curs.x);
	} else if (p->cx >= 0)
		wmove(stdscr, p->cy, p->cx);
	refresh();
	record_screen(ci->home);
	return 1;
}

static void ncurses_start(struct pane *p safe)
{
	struct display_data *dd = p->data;
	int rows, cols;

	start_color();
	use_default_colors();
	raw();
	noecho();
	nonl();
	timeout(0);
	set_escdelay(100);
	intrflush(stdscr, FALSE);
	keypad(stdscr, TRUE);
	mousemask(BUTTON1_PRESSED | BUTTON1_RELEASED |
		  BUTTON2_PRESSED | BUTTON2_RELEASED |
		  BUTTON3_PRESSED | BUTTON3_RELEASED |
		  BUTTON4_PRESSED | BUTTON4_RELEASED |
		  BUTTON5_PRESSED | BUTTON5_RELEASED |
		  BUTTON_CTRL | BUTTON_SHIFT | BUTTON_ALT |
		  REPORT_MOUSE_POSITION, NULL);
	mouseinterval(0);
	if (dd->is_xterm) {
		/* Enable bracketed-paste */
		fprintf(dd->scr_file, "\033[?2004h");
		fflush(dd->scr_file);
	}

	getmaxyx(stdscr, rows, cols);
	pane_resize(p, 0, 0, cols, rows);
}

static struct pane *ncurses_init(struct pane *ed safe,
				 const char *tty, const char *term)
{
	SCREEN *scr;
	struct pane *p;
	struct display_data *dd;
	char *area;
	FILE *f;

	set_screen(NULL);
	if (tty && strcmp(tty, "-") != 0)
		f = fopen(tty, "r+");
	else
		f = fdopen(1, "r+");
	if (!f)
		return NULL;
	scr = newterm(term, f, f);
	if (!scr)
		return NULL;

	p = pane_register(ed, 1, &ncurses_handle.c);
	if (!p)
		return NULL;
	dd = p->data;
	dd->scr = scr;
	dd->scr_file = f;
	dd->is_xterm = (term && strstarts(term, "xterm"));

	set_screen(p);

	ncurses_start(p);

	area = dd->attr_buf;
	dd->rs1 = tgetstr("rs1", &area);
	if (!dd->rs1)
		dd->rs1 = tgetstr("is1", &area);
	dd->rs2 = tgetstr("rs2", &area);
	if (!dd->rs2)
		dd->rs2 = tgetstr("is2", &area);
	dd->rs3 = tgetstr("rs3", &area);
	if (!dd->rs3)
		dd->rs3 = tgetstr("is3", &area);
	dd->clear = tgetstr("clear", &area);

	call("editor:request:all-displays", p);
	if (!prepare_recrep(p)) {
		call_comm("event:read", p, &input_handle, fileno(f));
		if (!tty || strcmp(tty, "-") == 0)
			call_comm("event:signal", p, &handle_winch, SIGWINCH);
	}
	return p;
}

REDEF_CMD(handle_winch)
{
	struct pane *p = ci->home;
	struct display_data *dd = p->data;
	struct winsize size;
	ioctl(fileno(dd->scr_file), TIOCGWINSZ, &size);
	set_screen(p);
	resize_term(size.ws_row, size.ws_col);

	pane_resize(p, 0, 0, size.ws_row, size.ws_col);
	return 1;
}

DEF_CMD(force_redraw)
{
	struct pane *p = ci->home;

	set_screen(p);

	/* full reset, as mosh sometimes gets confused */
	ncurses_stop(p);
	ncurses_start(p);

	clearok(curscr, 1);
	return 1;
}

static void ncurses_text(struct pane *p safe, struct pane *display safe,
			 wchar_t ch, int attr, int pair,
			 short x, short y, short cursor)
{
	PANEL *pan;
	struct pane *p2;
	cchar_t cc = {};

	if (x < 0 || y < 0)
		return;

	set_screen(display);
	if (cursor) {
		if (pane_has_focus(p->z < 0 ? p->parent : p)) {
			/* Cursor is in-focus */
			struct xy curs = pane_mapxy(p, display, x, y, False);
			display->cx = curs.x;
			display->cy = curs.y;
		} else
			/* Cursor here, but not focus */
			attr = make_cursor(attr);
	}
	cc.attr = attr;
	cc.ext_color = pair;
	cc.chars[0] = ch;

	p2 = p;
	pan = pane_panel(p2, NULL);
	while (!pan && p2->parent != p2) {
		p2 = p2->parent;
		pan = pane_panel(p2, NULL);
	}
	if (pan) {
		struct xy xy = pane_mapxy(p, p2, x, y, False);
		mvwadd_wch(panel_window(pan), xy.y, xy.x, &cc);
	}
}

static struct namelist {
	wint_t key;
	char *name;
} key_names[] = {
	{KEY_DOWN, ":Down"},
	{KEY_UP, ":Up"},
	{KEY_LEFT, ":Left"},
	{KEY_RIGHT, ":Right"},
	{KEY_HOME, ":Home"},
	{KEY_BACKSPACE, ":Backspace"},
	{KEY_DL, ":DelLine"},
	{KEY_IL, ":InsLine"},
	{KEY_DC, ":Del"},
	{KEY_IC, ":Ins"},
	{KEY_ENTER, ":Enter"},
	{KEY_END, ":End"},

	{KEY_NPAGE, ":Next"},
	{KEY_PPAGE, ":Prior"},

	{KEY_SDC, ":S:Del"},
	{KEY_SDL, ":S:DelLine"},
	{KEY_SEND, ":S:End"},
	{KEY_SHOME, ":S:Home"},
	{KEY_SLEFT, ":S:Left"},
	{KEY_SRIGHT, ":S:Right"},
	{KEY_BTAB, ":S:Tab"},

	{  0521, ":S:Up"},
	{  0520, ":S:Down"},
	{  0616, ":S:Prior"},
	{  0614, ":S:Next"},
	{ 01041, ":S:Home"},
	{ 01060, ":S:End"},
	{ 01066, ":S:Prior"},
	{ 01015, ":S:Next"},

	{ 01027, ":A:S:Home"},
	{ 01022, ":A:S:End"},
	{ 01046, ":A:S:Prior"},
	{ 01047, ":A:S:Next"}, // ??

	{ 01052, ":A:Prior"},
	{ 01045, ":A:Next"},
	{ 01026, ":A:Home"},
	{ 01021, ":A:End"},
	{ 01065, ":A:Up"},
	{ 01014, ":A:Down"},
	{ 01040, ":A:Left"},
	{ 01057, ":A:Right"},
	{ 00411, ":F1"},
	{ 00412, ":F2"},
	{ 00413, ":F3"},
	{ 00414, ":F4"},
	{ 00415, ":F5"},
	{ 00416, ":F6"},
	{ 00417, ":F7"},
	{ 00420, ":F8"},
	{ 00421, ":F9"},
	{ 00422, ":F10"},
	{ 00423, ":F11"},
	{ 00424, ":F12"},
	{ 00425, ":S:F1"},
	{ 00426, ":S:F2"},
	{ 00427, ":S:F3"},
	{ 00430, ":S:F4"},
	{ 00431, ":S:F5"},
	{ 00432, ":S:F6"},
	{ 00433, ":S:F7"},
	{ 00434, ":S:F8"},
	{ 00435, ":S:F9"},
	{ 00436, ":S:F10"},
	{ 00437, ":S:F11"},
	{ 00440, ":S:F12"},
	{ 01114, ":Focus-in"},
	{ 01115, ":Focus-out"},
	{0, NULL}
}, char_names[] = {
	{'\e', ":ESC"},
	{'\n', ":LF"},
	{'\r', ":Enter"},
	{'\t', ":Tab"},
	{'\177', ":Delete"},
	{'\0', ":C- "},
	{0, NULL}
};

static char *find_name (struct namelist *l safe, wint_t c)
{
	int i;
	for (i = 0; l[i].name; i++)
		if (l[i].key == c)
			return l[i].name;
	return NULL;
}

static void send_key(int keytype, wint_t c, int alt, struct pane *p safe)
{
	struct display_data *dd = p->data;
	char *n;
	char buf[100];/* FIXME */
	char t[5];
	char *a = alt ? ":A" : "";

	if (keytype == KEY_CODE_YES) {
		n = find_name(key_names, c);
		if (!n) {
			LOG("Unknown ncurses key 0o%o", c);
			sprintf(buf, "%sNcurs-%o", a, c);
		} else if (strstarts(n, ":Focus-"))
			/* Ignore focus changes for now */
			buf[0] = 0;
		else
			strcat(strcpy(buf, a), n);
	} else {
		n = find_name(char_names, c);
		if (n)
			sprintf(buf, "%s%s", a, n);
		else if (c < ' ' || c == 0x7f)
			sprintf(buf, "%s:C-%c",
				a, c ^ 64);
		else
			sprintf(buf, "%s-%s", a, put_utf8(t, c));
	}

	dd->last_event = time(NULL);
	if (buf[0]) {
		record_key(p, buf);
		call("Keystroke", p, 0, NULL, buf);
	}
}

static void do_send_mouse(struct pane *p safe, int x, int y, char *cmd safe,
			  int button, char *mod, int type)
{
	int ret;
	struct display_data *dd = p->data;

	record_mouse(p, cmd, x, y);
	ret = call("Mouse-event", p, button, NULL, cmd, type, NULL, mod, x, y);
	if (type == 1 && !dd->report_position) {
		if (dd->is_xterm) {
			fprintf(dd->scr_file, "\033[?1002h");
			fflush(dd->scr_file);
		}
		dd->report_position = 1;
	} else if (type == 3 && ret <= 0) {
		if (dd->is_xterm) {
			fprintf(dd->scr_file, "\033[?1002l");
			fflush(dd->scr_file);
		}
		dd->report_position = 0;
	}
}

static void send_mouse(MEVENT *mev safe, struct pane *p safe)
{
	struct display_data *dd = p->data;
	int x = mev->x;
	int y = mev->y;
	int b;
	char buf[100];

	/* MEVENT has lots of bits.  We want a few numbers */
	for (b = 1 ; b <= (NCURSES_MOUSE_VERSION <= 1 ? 3 : 5); b++) {
		mmask_t s = mev->bstate;
		char *action;
		int modf = 0;
		char *mod = "";

		if (s & BUTTON_SHIFT) modf |= 1;
		if (s & BUTTON_CTRL)  modf |= 2;
		if (s & BUTTON_ALT)   modf |= 4;
		switch (modf) {
		case 0: mod = ""; break;
		case 1: mod = ":S"; break;
		case 2: mod = ":C"; break;
		case 3: mod = ":C:S"; break;
		case 4: mod = ":A"; break;
		case 5: mod = ":A:S"; break;
		case 6: mod = ":A:C"; break;
		case 7: mod = ":A:C:S"; break;
		}
		if (BUTTON_PRESS(s, b))
			action = "%s:Press-%d";
		else if (BUTTON_RELEASE(s, b)) {
			action = "%s:Release-%d";
			/* Modifiers only reported on button Press */
			mod = "";
		} else
			continue;
		snprintf(buf, sizeof(buf), action, mod, b);
		dd->last_event = time(NULL);
		do_send_mouse(p, x, y, buf, b, mod, BUTTON_PRESS(s,b) ? 1 : 2);
	}
	if ((mev->bstate & REPORT_MOUSE_POSITION) &&
	    dd->report_position)
		/* Motion doesn't update last_event */
		do_send_mouse(p, x, y, ":Motion", 0, "", 3);
}

static void paste_start(struct pane *home safe)
{
	struct display_data *dd = home->data;

	dd->paste_start = time(NULL);
	buf_init(&dd->paste_buf);
}

static void paste_flush(struct pane *home safe)
{
	struct display_data *dd = home->data;

	if (!dd->paste_start)
		return;
	free(dd->paste_latest);
	dd->paste_latest = buf_final(&dd->paste_buf);
	if (dd->paste_buf.len > 0)
		dd->paste_pending = 1;
	dd->paste_start = 0;
}

static bool paste_recv(struct pane *home safe, int is_keycode, wint_t ch)
{
	struct display_data *dd = home->data;
	time_t now;
	if (dd->paste_start == 0)
		return False;
	now = time(NULL);
	if (dd->paste_start < now || dd->paste_start > now + 2 ||
	    is_keycode != OK || ch == KEY_MOUSE) {
		/* time to close */
		paste_flush(home);
		return False;
	}
	if (ch == '\r')
		/* I really don't want carriage-returns... */
		ch = '\n';
	buf_append(&dd->paste_buf, ch);
	if (ch == '~' && dd->paste_buf.len >= 6 &&
	    strcmp("\e[201~",
		   buf_final(&dd->paste_buf) + dd->paste_buf.len - 6) == 0) {
		dd->paste_buf.len -= 6;
		paste_flush(home);
	}
	return True;
}

DEF_CMD(nc_get_paste)
{
	struct display_data *dd = ci->home->data;

	comm_call(ci->comm2, "cb", ci->focus,
		  dd->paste_start, NULL, dd->paste_latest);
	return 1;
}

REDEF_CMD(input_handle)
{
	struct pane *p = ci->home;
	struct display_data *dd = p->data;
	static const char paste_seq[] = "\e[200~";
	wint_t c;
	int is_keycode;
	int have_escape = 0;
	int i;

	wait_for(dd);
	set_screen(p);
	while ((is_keycode = get_wch(&c)) != ERR) {
		if (dd->suspended && c != KEY_MOUSE) {
			dd->suspended = False;
			doupdate();
			call_comm("event:free", p, &ns_resume);
			/* swallow the key */
			continue;
		}
		if (paste_recv(p, is_keycode, c))
			continue;
		if (c == KEY_MOUSE) {
			MEVENT mev;
			paste_flush(p);
			while (getmouse(&mev) != ERR) {
				if (dd->paste_pending &&
				    mev.bstate == REPORT_MOUSE_POSITION) {
					/* xcfe-terminal is a bit weird.
					 * It captures middle-press to
					 * sanitise the paste, but lets
					 * middle-release though. It comes
					 * here as REPORT_MOUSE_POSTION
					 * and we can use that to find the
					 * position of the paste.
					 * '6' is an unused button, and
					 * ensures lib-input doesn't expect
					 * matching press/release
					 */
					call("Mouse-event", ci->home,
					     1, NULL, ":Paste",
					     6, NULL, NULL, mev.x, mev.y);
					dd->paste_pending = 0;
				}
				send_mouse(&mev, p);
			}
		} else if (c == (wint_t)paste_seq[have_escape]) {
			have_escape += 1;
			if (!paste_seq[have_escape]) {
				paste_start(p);
				have_escape = 0;
			}
		} else if (have_escape == 1) {
			send_key(is_keycode, c, 1, p);
			have_escape = 0;
		} else if (have_escape) {
			send_key(OK, paste_seq[1], 1, p);
			for (i = 2; i < have_escape; i++)
				send_key(OK, paste_seq[i], 0, p);
			send_key(is_keycode, c, 0, p);
			have_escape = 0;
		} else {
			send_key(is_keycode, c, 0, p);
		}
		/* Don't know what other code might have done,
		 * so re-set the screen
		 */
		set_screen(p);
	}
	if (have_escape == 1)
		send_key(is_keycode, '\e', 0, p);
	else if (have_escape > 1) {
		send_key(OK, paste_seq[1], 1, p);
		for (i = 2; i < have_escape; i++)
			send_key(OK, paste_seq[i], 0, p);
	}
	if (dd->paste_pending == 2) {
		/* no mouse event to give postion, so treat as keyboard */
		call("Keystroke", ci->home, 0, NULL, ":Paste");
		dd->paste_pending = 0;
	} else if (dd->paste_pending == 1) {
		/* Wait for possible mouse-position update. */
		dd->paste_pending = 2;
		call_comm("event:timer", p, &input_handle, 200);
	}
	return 1;
}

DEF_CMD(display_ncurses)
{
	struct pane *p;
	struct pane *ed = pane_root(ci->focus);
	const char *tty = ci->str;
	const char *term = ci->str2;

	if (!term)
		term = "xterm-256color";

	p = ncurses_init(ed, tty, term);
	if (p)
		p = call_ret(pane, "editor:activate-display", p);
	if (p && ci->focus != ed)
		/* Assume ci->focus is a document */
		p = home_call_ret(pane, ci->focus, "doc:attach-view", p, 1);
	if (p)
		return comm_call(ci->comm2, "callback:display", p);

	return Efail;
}

void edlib_init(struct pane *ed safe)
{
	call_comm("global-set-command", ed, &display_ncurses, 0, NULL,
		  "attach-display-ncurses");

	nc_map = key_alloc();
	key_add(nc_map, "window:refresh", &force_redraw);
	key_add(nc_map, "window:close", &nc_close_display);
	key_add(nc_map, "window:external-viewer", &nc_external_viewer);
	key_add(nc_map, "Close", &nc_close);
	key_add(nc_map, "Draw:clear", &nc_clear);
	key_add(nc_map, "Draw:text-size", &nc_text_size);
	key_add(nc_map, "Draw:text", &nc_draw_text);

	key_add(nc_map, "Draw:image", &nc_draw_image);
	key_add(nc_map, "Draw:image-size", &nc_image_size);

	key_add(nc_map, "Refresh:size", &nc_refresh_size);
	key_add(nc_map, "Refresh:postorder", &nc_refresh_post);
	key_add(nc_map, "Paste:get", &nc_get_paste);
	key_add(nc_map, "all-displays", &nc_notify_display);
	key_add(nc_map, "Sig:Winch", &handle_winch);
	key_add(nc_map, "Notify:Close", &nc_pane_close);
}
