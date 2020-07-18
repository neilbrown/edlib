/*
 * Copyright Neil Brown ©2015-2020 <neil@brown.name>
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
#include <time.h>
#include <curses.h>
#include <panel.h>
#include <string.h>
#include <locale.h>
#include <ctype.h>
#include <signal.h>
#include <sys/ioctl.h>

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
	char			*noclose;
	struct col_hash		*col_hash;
	int			report_position;
	long			last_event;
	#ifdef RECORD_REPLAY
	FILE			*log;
	FILE			*input;
	char			last_screen[MD5_DIGEST_SIZE*2+1];
	char			next_screen[MD5_DIGEST_SIZE*2+1];
	/* The next event to generate when idle */
	enum { DoNil, DoMouse, DoKey, DoCheck, DoClose} next_event;
	char			event_info[30];
	struct xy		event_pos;
	#endif
};

static SCREEN *current_screen;
static void ncurses_text(struct pane *p safe, struct pane *display safe,
			 wchar_t ch, int attr, short x, short y, short cursor);
static PANEL * safe pane_panel(struct pane *p safe, struct pane *home);
DEF_CMD(input_handle);
DEF_CMD(handle_winch);
static struct map *nc_map;
DEF_LOOKUP_CMD(ncurses_handle, nc_map);

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
DEF_CMD(abort_replay);

static int parse_event(struct pane *p safe);
static int prepare_recrep(struct pane *p safe)
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
		return 1;
	}
	return 0;
}

static void close_recrep(struct pane *p safe)
{
	struct display_data *dd = p->data;

	if (dd->log) {
		fprintf(dd->log, "Close\n");
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
	if (dd->log) {
		fprintf(dd->log, "Display %d,%d %s", p->w, p->h, out);
		strcpy(dd->last_screen, out);
		if (p->cx >= 0)
			fprintf(dd->log, " %d,%d", p->cx, p->cy);
		fprintf(dd->log, "\n");
	}
	if (dd->input && dd->next_event == DoCheck) {
		call_comm("event:free", p, &abort_replay);
//		if (strcmp(dd->last_screen, dd->next_screen) != 0)
//			dd->next_event = DoClose;
		call_comm("editor-on-idle", p, &next_evt);
	}
}

static inline int match(char *line safe, char *w safe)
{
	return strncmp(line, w, strlen(w)) == 0;
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

REDEF_CMD(abort_replay)
{
	struct display_data *dd = ci->home->data;

	dd->next_event = DoClose;
	return next_evt_func(ci);
}

static int parse_event(struct pane *p safe)
{
	struct display_data *dd = p->data;
	char line[80];

	line[79] = 0;
	dd->next_event = DoNil;
	if (!dd->input ||
	    fgets(line, sizeof(line)-1, dd->input) == NULL)
		;
	else if (match(line, "Key ")) {
		if (!copy_quote(line+4, dd->event_info))
			return 0;
		dd->next_event = DoKey;
	} else if (match(line, "Mouse ")) {
		char *f = copy_quote(line+6, dd->event_info);
		if (!f)
			return 0;
		f = get_coord(f, &dd->event_pos);
		if (!f)
			return 0;
		dd->next_event = DoMouse;
	} else if (match(line, "Display ")) {
		char *f = get_coord(line+8, &dd->event_pos);
		if (!f)
			return 0;
		f = get_hash(f, dd->next_screen);
		dd->next_event = DoCheck;
	} else if (match(line, "Close")) {
		dd->next_event = DoClose;
	}

	if (dd->next_event != DoCheck)
		call_comm("editor-on-idle", p, &next_evt);
	else
		call_comm("event:timer", p, &abort_replay, 10*1000);
	return 1;
}

REDEF_CMD(next_evt)
{
	struct pane *p = ci->home;
	struct display_data *dd = p->data;
	int button = 0, type = 0;
	char *delay;

	delay = getenv("EDLIB_REPLAY_DELAY");
	if (delay && dd->next_event != DoCheck)
		usleep(atoi(delay)*1000);

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
static inline int  prepare_recrep(struct pane *p safe) {return 0;}
static inline void record_key(struct pane *p safe, char *key) {}
static inline void record_mouse(struct pane *p safe, char *key safe,
				int x, int y) {}
static inline void record_screen(struct pane *p safe) {}
static inline void close_recrep(struct pane *p safe) {}
#endif

DEF_CMD(cnt_disp)
{
	struct call_return *cr = container_of(ci->comm, struct call_return, c);

	cr->i += 1;
	return 1;
}

DEF_CMD(nc_close_display)
{
	/* If this is only display, then refuse to close this one */
	struct call_return cr;
	struct display_data *dd = ci->home->data;

	if (dd->noclose) {
		call("Message", ci->focus, 0, NULL, dd->noclose);
		return 1;
	}

	cr.c = cnt_disp;
	cr.i = 0;
	call_comm("editor:notify:all-displays", ci->focus, &cr.c);
	if (cr.i > 1)
		pane_close(ci->home);
	else
		call("Message", ci->focus, 0, NULL,
		     "Cannot close only window.");
	return 1;
}

DEF_CMD(nc_set_noclose)
{
	struct display_data *dd = ci->home->data;

	free(dd->noclose);
	dd->noclose = NULL;
	if (ci->str)
		dd->noclose = strdup(ci->str);
	return 1;
}

static void ncurses_end(struct pane *p safe)
{
	set_screen(p);
	close_recrep(p);
	nl();
	endwin();
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
		     const char *attrs)
{
	struct display_data *dd = home->data;
	int attr = 0;
	char tmp[40];
	const char *a;
	PANEL *pan = NULL;
	int fg = COLOR_BLACK;
	int bg = COLOR_WHITE+8;

	set_screen(home);
	do {
		p = p->parent;
	} while (p->parent != p &&(pan = pane_panel(p, NULL)) == NULL);
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
	a = attrs;
	while (a && *a) {
		const char *c;
		if (*a == ',') {
			a++;
			continue;
		}
		c = strchr(a, ',');
		if (!c)
			c = a+strlen(a);
		strncpy(tmp, a, c-a);
		tmp[c-a] = 0;
		if (strcmp(tmp, "inverse")==0) attr |= A_STANDOUT;
		else if (strcmp(tmp, "bold")==0) attr |= A_BOLD;
		else if (strcmp(tmp, "underline")==0) attr |= A_UNDERLINE;
		else if (strncmp(tmp, "fg:", 3) == 0) {
			struct call_return cr =
				call_ret(all, "colour:map", home,
					 0, NULL, tmp+3);
			int rgb[3] = {cr.i, cr.i2, cr.x};
			fg = find_col(dd, rgb);
		} else if (strncmp(tmp, "bg:", 3) == 0) {
			struct call_return cr =
				call_ret(all, "colour:map", home,
					 0, NULL, tmp+3);
			int rgb[3] = {cr.i, cr.i2, cr.x};
			bg = find_col(dd, rgb);
		}
		a = c;
	}
	if (fg != COLOR_BLACK || bg != COLOR_WHITE+8)
		attr |= COLOR_PAIR(to_pair(dd, fg, bg));
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
	return 0;
}

DEF_CMD(nc_close)
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
	}
	return 1;
}

static PANEL * safe pane_panel(struct pane *p safe, struct pane *home)
{
	PANEL *pan = NULL;
	struct xy xy;

	while ((pan = panel_above(pan)) != NULL)
		if (panel_userptr(pan) == p)
			return pan;

	if (!home)
		return pan;

	xy = pane_mapxy(p, home, 0, 0, False);

	pan = new_panel(newwin(p->h, p->w, xy.y, xy.x));
	set_panel_userptr(pan, p);
	pane_add_notify(home, p, "Notify:Close");

	return pan;
}

DEF_CMD(nc_clear)
{
	struct pane *p = ci->home;
	int attr = cvt_attrs(ci->focus, p, ci->str2?:ci->str);
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
	wbkgdset(win, attr);
	werase(win);

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
		if (wc == WEOF || wc == WERR)
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
	int attr = cvt_attrs(ci->focus, p, ci->str2);
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
			ncurses_text(ci->focus, p, wc, attr, x, y, 1);
		else
			ncurses_text(ci->focus, p, wc, attr, x, y, 0);
		x += width;
	}
	if (str == ci->str + cursor_offset)
		ncurses_text(ci->focus, p, ' ', 0, x, y, 1);
	pane_damaged(p, DAMAGED_POSTORDER);
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
	struct pane *p1;
	PANEL *pan, *pan2;

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

		if (p1->abs_z <= p2->abs_z)
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
		struct xy src, dest, area;

		p1 = (void*)panel_userptr(pan);
		if (!p1)
			continue;
		dest = pane_mapxy(p1, p, 0, 0, True);
		area = pane_mapxy(p1, p, p->w, p->h, True);
		src = pane_mapxy(p1, p, 0, 0, False);
		src.x = dest.x - src.x;
		src.y = dest.y - src.y;
		win = panel_window(pan);
		copywin(win, stdscr, src.y, src.x,
			dest.y, dest.x, area.y-1, area.x-1, 0);
	}
	/* place the cursor */
	p1 = pane_leaf(p);
	pan = NULL;
	while (p1 != p && (pan = pane_panel(p1, NULL)) == NULL)
		p1 = p1->parent;
	if (pan) {
		struct xy curs = pane_mapxy(p1, p, p1->cx, p1->cy, False);
		wmove(stdscr, curs.y, curs.x);
	}
	refresh();
	record_screen(ci->home);
	return 1;
}

static struct pane *ncurses_init(struct pane *ed,
				 const char *tty, const char *term)
{
	SCREEN *scr;
	struct pane *p;
	struct display_data *dd;
	int rows, cols;
	FILE *f;

	set_screen(NULL);
	if (tty)
		f = fopen(tty, "r+");
	else
		f = fdopen(1, "r+");
	if (!f)
		return NULL;
	scr = newterm(term, f, f);
	if (!scr)
		return NULL;

	alloc(dd, pane);
	dd->scr = scr;
	dd->scr_file = f;
	dd->is_xterm = (term && strncmp(term, "xterm", 5) == 0);

	p = pane_register(ed, 1, &ncurses_handle.c, dd);
	if (!p) {
		unalloc(dd, pane);
		return NULL;
	}
	set_screen(p);

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
	mouseinterval(10);

	getmaxyx(stdscr, rows, cols);
	pane_resize(p, 0, 0, cols, rows);

	call("editor:request:all-displays", p);
	if (!prepare_recrep(p)) {
		call_comm("event:read", p, &input_handle, fileno(f));
		if (!tty)
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
	clearok(curscr, 1);
	return 1;
}

static void ncurses_text(struct pane *p safe, struct pane *display safe,
			 wchar_t ch, int attr, short x, short y, short cursor)
{
	PANEL *pan;
	struct pane *p2;
	cchar_t cc = {};

	if (x < 0 || y < 0)
		return;
	if (cursor) {
		p2 = p;
		cursor = 2;
		while (p2->parent != p2 && p2 != display) {
			if (p2->parent->focus != p2 && p2->z >= 0)
				cursor = 1;
			p2 = p2->parent;
		}
	}

	set_screen(display);
	if (cursor == 2) {
		/* Cursor is in-focus */
		struct xy curs = pane_mapxy(p, display, x, y, False);
		display->cx = curs.x;
		display->cy = curs.y;
	}
	if (cursor == 1)
		/* Cursor here, but not focus */
		attr = make_cursor(attr);
	cc.attr = attr;
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
	{KEY_BACKSPACE, ":Backspace\037:C-H\037:C-h"},
	{KEY_DL, ":DelLine"},
	{KEY_IL, ":InsLine"},
	{KEY_DC, ":Del"},
	{KEY_IC, ":Ins"},
	{KEY_ENTER, ":Enter\037:C-M\037:C-m"},
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

	{ 01057, ":M:Prior"},
	{ 01051, ":M:Next"},
	{ 01072, ":M:Up"},
	{ 01061, ":M:Down"},
	{ 01042, ":M:Left"},
	{ 01064, ":M:Right"},
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

static void send_key(int keytype, wint_t c, int meta, struct pane *p safe)
{
	struct display_data *dd = p->data;
	char *n;
	char buf[100];/* FIXME */
	char t[5];
	char *m = meta ? ":M" : "";

	if (keytype == KEY_CODE_YES) {
		n = find_name(key_names, c);
		if (!n)
			sprintf(buf, "%sNcurs-%o", m, c);
		else
			strcat(strcpy(buf, m), n);
	} else {
		n = find_name(char_names, c);
		if (n)
			sprintf(buf, "%s%s\037%s:C-%c\037%s:C-%c",
				m, n,
				m, c+64,
				m, c+96);
		else if (c < ' ')
			sprintf(buf, "%s:C-%c\037%s:C-%c",
				m, c+64, m, c+96);
		else
			sprintf(buf, "%s-%s", m, put_utf8(t, c));
	}

	dd->last_event = time(NULL);
	record_key(p, buf);
	call("Keystroke", p, 0, NULL, buf);
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
	} else if (type == 3 && !ret) {
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
		case 4: mod = ":M"; break;
		case 5: mod = ":M:S"; break;
		case 6: mod = ":M:C"; break;
		case 7: mod = ":M:C:S"; break;
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

REDEF_CMD(input_handle)
{
	struct pane *p = ci->home;

	wint_t c;
	int is_keycode;
	int have_escape = 0;

	if (!(void*)p->data)
		/* already closed */
		return 0;
	set_screen(p);
	while ((is_keycode = get_wch(&c)) != ERR) {
		if (c == KEY_MOUSE) {
			MEVENT mev;
			while (getmouse(&mev) != ERR)
				send_mouse(&mev, p);
		} else if (have_escape) {
			send_key(is_keycode, c, 1, p);
			have_escape = 0;
		} else if (c == '\e')
			have_escape = 1;
		else
			send_key(is_keycode, c, 0, p);
		/* Don't know what other code might have done,
		 * so re-set the screen
		 */
		set_screen(p);
	}
	if (have_escape)
		send_key(is_keycode, '\e', 0, p);
	return 1;
}

DEF_CMD(display_ncurses)
{
	struct pane *p;
	char *term;

	term = pane_attr_get(ci->focus, "TERM");
	if (!term)
		term = getenv("TERM");
	if (!term)
		term = "xterm-256color";

	p = ncurses_init(ci->focus, ci->str, term);
	if (p) {
		struct pane *p2 = call_ret(pane, "attach-x11selection", p);
		if (p2)
			p = p2;
		return comm_call(ci->comm2, "callback:display", p);
	}
	return Efail;
}

void edlib_init(struct pane *ed safe)
{
	call_comm("global-set-command", ed, &display_ncurses, 0, NULL,
		  "attach-display-ncurses");

	nc_map = key_alloc();
	key_add(nc_map, "Display:refresh", &force_redraw);
	key_add(nc_map, "Display:close", &nc_close_display);
	key_add(nc_map, "Display:set-noclose", &nc_set_noclose);
	key_add(nc_map, "Close", &nc_close);
	key_add(nc_map, "Free", &edlib_do_free);
	key_add(nc_map, "pane-clear", &nc_clear);
	key_add(nc_map, "text-size", &nc_text_size);
	key_add(nc_map, "Draw:text", &nc_draw_text);
	key_add(nc_map, "Refresh:size", &nc_refresh_size);
	key_add(nc_map, "Refresh:postorder", &nc_refresh_post);
	key_add(nc_map, "all-displays", &nc_notify_display);
	key_add(nc_map, "Sig:Winch", &handle_winch);
	key_add(nc_map, "Notify:Close", &nc_pane_close);
}
