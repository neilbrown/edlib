/*
 * Copyright Neil Brown ©2015-2019 <neil@brown.name>
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
#include <curses.h>
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
	struct xy		cursor;
	char			*noclose;
	struct col_hash		*col_hash;
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
static void ncurses_clear(struct pane *p safe, struct pane *display safe,
			  int attr, short x, short y, short w, short h);
static void ncurses_text(struct pane *p safe, struct pane *display safe,
			 wchar_t ch, int attr, short x, short y, short cursor);
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
	uint16_t buf[CCHARW_MAX+4];
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
			short color;
			int l;

			mvin_wch(r,c,&cc);
			getcchar(&cc, wc, &a, &color, NULL);
			buf[0] = htole16(color);
			for (l = 0; l < CCHARW_MAX && wc[l]; l++)
				buf[l+2] = htole16(wc[l]);
			buf[1] = htole16(l);
			md5_update(&ctx, (uint8_t*)buf, (l+2) * 2);
		}
	md5_final_txt(&ctx, out);
	if (dd->log) {
		fprintf(dd->log, "Display %d,%d %s", p->w, p->h, out);
		strcpy(dd->last_screen, out);
		if (dd->cursor.x >= 0)
			fprintf(dd->log, " %d,%d", dd->cursor.x, dd->cursor.y);
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

	switch(dd->next_event) {
	case DoKey:
		record_key(p, dd->event_info);
		call("Keystroke", p, 0, NULL, dd->event_info);
		break;
	case DoMouse:
		record_mouse(p, dd->event_info, dd->event_pos.x, dd->event_pos.y);
		call("Mouse-event", p, 0, NULL, dd->event_info, 0, NULL, NULL,
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
static inline void record_mouse(struct pane *p safe, char *key safe, int x, int y) {}
static inline void record_screen(struct pane *p safe) {}
static inline void close_recrep(struct pane *p safe) {}
#endif

DEF_CMD(nc_refresh)
{
	struct pane *p = ci->home;

	call("Sig:Winch", p);
	set_screen(p);
	clear();
	pane_damaged(p,  DAMAGED_SIZE);
	return 1;
}

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
	call_comm("Call:Notify:global-displays", ci->focus, &cr.c);
	if (cr.i > 1)
		pane_close(ci->home);
	else
		call("Message", ci->focus, 0, NULL, "Cannot close only window.");
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


static int cvt_attrs(struct pane *home safe, char *attrs)
{
	struct display_data *dd = home->data;

	int attr = 0;
	char tmp[40];
	char *a;
	int fg = COLOR_BLACK;
	int bg = COLOR_WHITE+8;

	if (!attrs)
		return 0;
	set_screen(home);
	a = attrs;
	while (a && *a) {
		char *c;
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
				call_ret(all, "colour:map", home, 0, NULL, tmp+3);
			int rgb[3] = {cr.i, cr.i2, cr.x};
			fg = find_col(dd, rgb);
		} else if (strncmp(tmp, "bg:", 3) == 0) {
			struct call_return cr =
				call_ret(all, "colour:map", home, 0, NULL, tmp+3);
			int rgb[3] = {cr.i, cr.i2, cr.x};
			bg = find_col(dd, rgb);
		}
		a = c;
	}
	if (fg != COLOR_BLACK || bg != COLOR_WHITE+8) {
		int p = to_pair(dd, fg, bg);
		attr |= COLOR_PAIR(p);
	}
	return attr;
}

static int make_cursor(int attr)
{
	return attr ^ A_UNDERLINE;
}

DEF_CMD(nc_notify_display)
{
	comm_call(ci->comm2, "callback:display", ci->home);
	return 0;
}

DEF_CMD(nc_close)
{
	struct pane *p = ci->home;
	struct display_data *dd = p->data;
	ncurses_end(p);
	hash_free(dd);
	free(dd);
	p->data = safe_cast NULL;
	return 1;
}

DEF_CMD(nc_clear)
{
	struct pane *p = ci->home;
	int attr = cvt_attrs(p, ci->str2?:ci->str);

	ncurses_clear(ci->focus, p, attr, 0, 0, 0, 0);
	pane_damaged(p, DAMAGED_POSTORDER);
	return 1;
}

DEF_CMD(nc_text_size)
{
	int max_space = ci->num;
	int max_bytes = 0;
	int size = 0;
	int offset = 0;
	mbstate_t mbs = {};
	char *str = ci->str;
	int len;

	if (!str)
		return Enoarg;
	len = strlen(str);
	while (str[offset] != 0) {
		wchar_t wc;
		int skip = mbrtowc(&wc, str+offset, len-offset, &mbs);
		if (skip < 0)
			break;
		offset += skip;
		skip = wcwidth(wc);
		if (skip < 0)
			break;
		size += skip;
		if (size <= max_space)
			max_bytes = offset;
	}
	return comm_call(ci->comm2, "callback:size", ci->focus,
			 max_bytes, NULL, NULL,
			 0, NULL, NULL, size, 1);
}

DEF_CMD(nc_draw_text)
{
	struct pane *p = ci->home;
	int attr = cvt_attrs(p, ci->str2);
	int cursor_offset = ci->num;
	short offset = 0;
	short x = ci->x, y = ci->y;
	mbstate_t mbs = {};
	char *str = ci->str;
	int len;

	if (!str)
		return Enoarg;
	set_screen(p);
	len = strlen(str);
	while (str[offset] != 0) {
		wchar_t wc;
		int skip = mbrtowc(&wc, str+offset, len-offset, &mbs);
		int width;
		if (skip < 0)
			break;
		width = wcwidth(wc);
		if (width < 0)
			break;
		if (cursor_offset >= offset &&
		    cursor_offset < offset + skip)
			ncurses_text(ci->focus, p, wc, attr, x, y, 1);
		else
			ncurses_text(ci->focus, p, wc, attr, x, y, 0);
		offset += skip;
		x += width;
	}
	if (offset == cursor_offset)
		ncurses_text(ci->focus, p, ' ', 0, x, y, 1);
	pane_damaged(p, DAMAGED_POSTORDER);
	return 1;
}

DEF_CMD(nc_refresh_size)
{
	struct pane *p = ci->home;

	set_screen(p);
	getmaxyx(stdscr, p->h, p->w);
	return 0;
}

DEF_CMD(nc_refresh_post)
{
	struct pane *p = ci->home;
	struct display_data *dd = p->data;
	set_screen(p);
	if (dd->cursor.x >= 0)
		move(dd->cursor.y, dd->cursor.x);
	refresh();
	record_screen(p);
	return 1;
}

static struct pane *ncurses_init(struct pane *ed, char *tty, char *term)
{
	SCREEN *scr;
	struct pane *p;
	struct display_data *dd;
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

	dd = calloc(1, sizeof(*dd));
	dd->scr = scr;
	dd->scr_file = f;
	dd->cursor.x = dd->cursor.y = -1;

	p = pane_register(ed, 0, &ncurses_handle.c, dd);
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
	mousemask(ALL_MOUSE_EVENTS, NULL);

	ASSERT(can_change_color());

	getmaxyx(stdscr, p->h, p->w);

	call("Request:Notify:global-displays", p);
	if (!prepare_recrep(p)) {
		call_comm("event:read", p, &input_handle, fileno(f));
		if (!tty)
			call_comm("event:signal", p, &handle_winch, SIGWINCH);
	}
	pane_damaged(p, DAMAGED_SIZE);
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

	clear();
	pane_damaged(p, DAMAGED_SIZE);
	return 1;
}

static void ncurses_clear(struct pane *p safe, struct pane *display safe,
			  int attr, short x, short y, short w, short h)
{
	short r, c;
	short w0, h0;

	if (w == 0)
		w = p->w - x;
	if (h == 0)
		h = p->h - y;
	pane_absxy(p, &x, &y, &w, &h);
	w0 = w; h0 = h;
	if (pane_masked(display, x, y, p->abs_z, &w0, &h0))
		w0 = h0 = 0;

	set_screen(display);
	attrset(attr);
	for (r = y; r < y+h; r++)
		for (c = x; c < x+w; c++)
			if ((r < y+h0 && c < x+w0) ||
			    !pane_masked(display, c, r, p->abs_z, NULL, NULL))
				mvaddch(r, c, ' ');
}

static void ncurses_text(struct pane *p safe, struct pane *display safe,
			 wchar_t ch, int attr, short x, short y, short cursor)
{
	struct display_data *dd;
	cchar_t cc = {};
	short w=1, h=1;

	if (x < 0 || y < 0)
		return;
	if (cursor && p->parent) {
		struct pane *p2 = p;
		cursor = 2;
		while (p2->parent && p2 != display) {
			if (p2->parent->focus != p2)
				cursor = 1;
			p2 = p2->parent;
		}
	}

	pane_absxy(p, &x, &y, &w, &h);
	if (w < 1 || h < 1)
		return;

	if (pane_masked(display, x, y, p->abs_z, NULL, NULL))
		return;

	dd = display->data;
	set_screen(display);
	if (cursor == 2) {
		/* Cursor is in-focus */
		dd->cursor.x = x;
		dd->cursor.y = y;
	}
	if (cursor == 1)
		/* Cursor here, but not focus */
		attr = make_cursor(attr);
	cc.attr = attr;
	cc.chars[0] = ch;

	mvadd_wch(y, x, &cc);
}

static struct namelist {
	wint_t key;
	char *name;
} key_names[] = {
	{KEY_DOWN, "Down"},
	{KEY_UP, "Up"},
	{KEY_LEFT, "Left"},
	{KEY_RIGHT, "Right"},
	{KEY_HOME, "Home"},
	{KEY_BACKSPACE, "Backspace\037C-Chr-H"},
	{KEY_DL, "DelLine"},
	{KEY_IL, "InsLine"},
	{KEY_DC, "Del"},
	{KEY_IC, "Ins"},
	{KEY_ENTER, "Enter\037C-Chr-M"},
	{KEY_END, "End"},

	{KEY_NPAGE, "Next"},
	{KEY_PPAGE, "Prior"},

	{KEY_SDC, "S-Del"},
	{KEY_SDL, "S-DelLine"},
	{KEY_SEND, "S-End"},
	{KEY_SHOME, "S-Home"},
	{KEY_SLEFT, "S-Left"},
	{KEY_SRIGHT, "S-Right"},
	{KEY_BTAB, "S-Tab"},

	{ 01057, "M-Prior"},
	{ 01051, "M-Next"},
	{ 01072, "M-Up"},
	{ 01061, "M-Down"},
	{ 01042, "M-Left"},
	{ 01064, "M-Right"},
	{0, NULL}
}, char_names[] = {
	{'\e', "ESC"},
	{'\n', "LF"},
	{'\r', "Enter"},
	{'\t', "Tab"},
	{'\177', "Delete"},
	{'\0', "C-Chr- "},
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

static void send_key(int keytype, wint_t c, struct pane *p safe)
{
	char *n;
	char buf[100];/* FIXME */

	if (keytype == KEY_CODE_YES) {
		n = find_name(key_names, c);
		if (!n)
			sprintf(buf, "Ncurs-%o", c);
		else
			strcpy(buf, n);
	} else {
		n = find_name(char_names, c);
		if (n)
			sprintf(buf, "%s\037C-Chr-%c\037C-Chr-%c",
			        n, c+64, c+96);
		else if (c < ' ')
			sprintf(buf, "C-Chr-%c\037C-Chr-%c",
			        c+64, c+96);
		else
			sprintf(buf, "Chr-%lc", c);
	}

	record_key(p, buf);
	call("Keystroke", p, 0, NULL, buf);
}

static void do_send_mouse(struct pane *p safe, int x, int y, char *cmd safe)
{
	record_mouse(p, cmd, x, y);
	call("Mouse-event", p, 0, NULL, cmd, 0, NULL, NULL, x, y);
}

static void send_mouse(MEVENT *mev safe, struct pane *p safe)
{
	int x = mev->x;
	int y = mev->y;
	int b;
	char buf[100];

	/* MEVENT has lots of bits.  We want a few numbers */
	for (b = 1 ; b <= (NCURSES_MOUSE_VERSION <= 1 ? 3 : 5); b++) {
		mmask_t s = mev->bstate;
		char *action;
		if (BUTTON_PRESS(s, b))
			action = "Press-%d";
		else if (BUTTON_RELEASE(s, b))
			action = "Release-%d";
		else if (BUTTON_CLICK(s, b))
			action = "Click-%d";
		else if (BUTTON_DOUBLE_CLICK(s, b))
			action = "DClick-%d";
		else if (BUTTON_TRIPLE_CLICK(s, b))
			action = "TClick-%d";
		else
			continue;
		sprintf(buf, action, b);
		do_send_mouse(p, x, y, buf);
	}
	if (mev->bstate & REPORT_MOUSE_POSITION)
		do_send_mouse(p, x, y, "MouseMove");
}

REDEF_CMD(input_handle)
{
	struct pane *p = ci->home;
	wint_t c;
	int is_keycode;

	if (!(void*)p->data)
		/* already closed */
		return 0;
	set_screen(p);
	while ((is_keycode = get_wch(&c)) != ERR) {
		if (c == KEY_MOUSE) {
			MEVENT mev;
			while (getmouse(&mev) != ERR)
				send_mouse(&mev, p);
		} else
			send_key(is_keycode, c, p);
		/* Don't know what other code might have done,
		 * so re-set the screen
		 */
		set_screen(p);
	}
	return 1;
}

DEF_CMD(display_ncurses)
{
	struct pane *p = ncurses_init(ci->focus, ci->str, ci->str2);
	if (p)
		return comm_call(ci->comm2, "callback:display", p);
	return Efail;
}

void edlib_init(struct pane *ed safe)
{
	call_comm("global-set-command", ed, &display_ncurses, 0, NULL, "attach-display-ncurses");

	nc_map = key_alloc();
	key_add(nc_map, "Display:refresh", &nc_refresh);
	key_add(nc_map, "Display:close", &nc_close_display);
	key_add(nc_map, "Display:set-noclose", &nc_set_noclose);
	key_add(nc_map, "Close", &nc_close);
	key_add(nc_map, "pane-clear", &nc_clear);
	key_add(nc_map, "text-size", &nc_text_size);
	key_add(nc_map, "Draw:text", &nc_draw_text);
	key_add(nc_map, "Refresh:size", &nc_refresh_size);
	key_add(nc_map, "Refresh:postorder", &nc_refresh_post);
	key_add(nc_map, "Notify:global-displays", &nc_notify_display);
	key_add(nc_map, "Sig:Winch", &handle_winch);
}
