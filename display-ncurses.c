/*
 * Copyright Neil Brown ©2015 <neil@brown.name>
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

#define _XOPEN_SOURCE
#define _XOPEN_SOURCE_EXTENDED
#define _DEFAULT_SOURCE

#include <stdlib.h>
#include <curses.h>
#include <string.h>
#include <locale.h>
#include <ctype.h>
#include <signal.h>
#include <sys/ioctl.h>

#include "core.h"

#ifdef RECORD_REPLAY
#include <stdio.h>
#include "md5.h"
#endif

#ifdef __CHECKER__
#undef NCURSES_OK_ADDR
#define NCURSES_OK_ADDR(p) ((void*)0 != NCURSES_CAST(const void *, (p)))
#endif

struct display_data {
	SCREEN			*scr;
	struct {int x,y;}	cursor;
	#ifdef RECORD_REPLAY
	FILE			*log;
	FILE			*input;
	#endif
};

static SCREEN *current_screen;
static void ncurses_clear(struct pane *p safe, struct pane *display safe,
			  int attr, int x, int y, int w, int h);
static void ncurses_text(struct pane *p safe, struct pane *display safe,
			 wchar_t ch, int attr, int x, int y, int cursor);

static void set_screen(SCREEN *scr)
{
	if (scr == current_screen)
		return;
	set_term(scr);
	current_screen = scr;
}

#ifdef RECORD_REPLAY
void prepare_recrep(struct pane *p safe)
{
	struct display_data *dd = p->data;
	char *name;

	name = getenv("EDLIB_RECORD");
	if (name)
		dd->log = fopen(name, "w");
}

void close_recrep(struct pane *p safe)
{
	struct display_data *dd = p->data;

	if (dd->log) {
		fprintf(dd->log, "Close\n");
		fclose(dd->log);
	}
}

void record_key(struct pane *p safe, char *key safe)
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

void record_mouse(struct pane *p safe, char *key safe, int x, int y)
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

void record_screen(struct pane *p safe)
{
	struct display_data *dd = p->data;
	struct md5_state ctx;
	uint16_t buf[CCHARW_MAX+4];
	char out[MD5_DIGEST_SIZE*2+1];
	int r,c;

	if (!dd->log)
		return;
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
	fprintf(dd->log, "Display %d,%d %s", p->w, p->h, out);
	if (dd->cursor.x >= 0)
		fprintf(dd->log, " %d,%d", dd->cursor.x, dd->cursor.y);
	fprintf(dd->log, "\n");
}
#else
static inline void prepare_recrep(struct pane *p safe) {}
static inline void record_key(struct pane *p safe, char *key) {}
static inline void record_mouse(struct pane *p safe, char *key safe, int x, int y) {}
static inline void record_screen(struct pane *p safe) {}
static inline void close_recrep(struct pane *p safe) {}
#endif

DEF_CMD(input_handle);
DEF_CMD(handle_winch);

DEF_CMD(nc_misc)
{
	struct pane *p = ci->home;

	if (strcmp(ci->key, "Display:refresh") == 0) {
		clear();
		pane_damaged(p,  DAMAGED_SIZE);
		return 1;
	}
	return 0;
}

static void ncurses_end(struct pane *p safe)
{
	close_recrep(p);
	nl();
	endwin();
}

static int cvt_attrs(char *attrs)
{
	int attr = 0;
	char tmp[40];
	char *a;

	if (!attrs)
		return 0;
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
		else if (strcmp(tmp, "fg:blue")  == 0) {
			init_pair(1, COLOR_BLUE, -1);
			attr |= COLOR_PAIR(1);
		} else if (strcmp(tmp, "fg:red")  == 0) {
			init_pair(2, COLOR_RED, -1);
			attr |= COLOR_PAIR(2);
		} else if (strcmp(tmp, "fg:grey")  == 0) {
			/* HORRIBLE HACK - MUST FIXME */
			init_pair(3, COLOR_YELLOW, -1);
			attr |= COLOR_PAIR(3);
		}
		a = c;
	}
	return attr;
}

static int make_cursor(int attr)
{
	return attr ^ A_STANDOUT;
}

DEF_CMD(ncurses_handle)
{
	struct pane *p = ci->home;
	struct display_data *dd = p->data;

	if (strncmp(ci->key, "Display:", 8) == 0)
		return nc_misc.func(ci);

	if (strcmp(ci->key, "Close") == 0) {
		ncurses_end(p);
		return 1;
	}
	if (strcmp(ci->key, "pane-clear") == 0) {
		int attr = cvt_attrs(ci->str2);
		ncurses_clear(ci->focus, p, attr, 0, 0, 0, 0);
		pane_damaged(p, DAMAGED_POSTORDER);
		return 1;
	}
	if (strcmp(ci->key, "text-size") == 0 && ci->str) {
		int max_space = ci->numeric;
		int max_bytes = 0;
		int size = 0;
		int offset = 0;
		mbstate_t mbs = {};
		char *str = ci->str;
		int len = strlen(str);
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
				 0, NULL, NULL, NULL, size, 1);
	}
	if (strcmp(ci->key, "Draw:text") == 0 && ci->str) {
		int attr = cvt_attrs(ci->str2);
		int cursor_offset = ci->numeric;
		int offset = 0;
		int x = ci->x, y = ci->y;
		mbstate_t mbs = {};
		char *str = ci->str;
		int len = strlen(str);
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
	if (strcmp(ci->key, "Refresh:size") == 0) {
		set_screen(dd->scr);
		getmaxyx(stdscr, p->h, p->w);
		return 0;
	}
	if (strcmp(ci->key, "Refresh:postorder") == 0) {
		set_screen(dd->scr);
		if (dd->cursor.x >= 0)
			move(dd->cursor.y, dd->cursor.x);
		refresh();
		record_screen(p);
		return 1;
	}
	return 0;
}

static struct pane *ncurses_init(struct pane *ed)
{
	WINDOW *w = initscr();
	struct pane *p;
	struct display_data *dd = calloc(1, sizeof(*dd));

	start_color();
	use_default_colors();
	raw();
	noecho();
	nonl();
	timeout(0);
	set_escdelay(100);
	intrflush(w, FALSE);
	keypad(w, TRUE);
	mousemask(ALL_MOUSE_EVENTS, NULL);

	dd->scr = NULL;
	dd->cursor.x = dd->cursor.y = -1;

	current_screen = NULL;
	p = pane_register(ed, 0, &ncurses_handle, dd, NULL);

	getmaxyx(stdscr, p->h, p->w);

	prepare_recrep(p);

	call_home(p, "event:read", p, 0, NULL, NULL, 0, NULL, NULL, &input_handle);
	call_home(p, "event:signal", p, SIGWINCH, NULL, NULL, 0, NULL, NULL, &handle_winch);
	pane_damaged(p, DAMAGED_SIZE);
	return p;
}

REDEF_CMD(handle_winch)
{
	struct pane *p = ci->home;
	struct winsize size;
	ioctl(1, TIOCGWINSZ, &size);
	resize_term(size.ws_row, size.ws_col);

	clear();
	pane_damaged(p, DAMAGED_SIZE);
	return 1;
}

static void ncurses_clear(struct pane *p safe, struct pane *display safe,
			  int attr, int x, int y, int w, int h)
{
	int r, c;
	struct display_data *dd;
	int w0, h0;

	if (w == 0)
		w = p->w - x;
	if (h == 0)
		h = p->h - y;
	pane_absxy(p, &x, &y, &w, &h);
	w0 = w; h0 = h;
	if (pane_masked(display, x, y, p->abs_z, &w0, &h0))
		w0 = h0 = 0;

	dd = display->data;
	set_screen(dd->scr);
	attrset(attr);
	for (r = y; r < y+h; r++)
		for (c = x; c < x+w; c++)
			if ((r < y+h0 && c < x+w0) ||
			    !pane_masked(display, c, r, p->abs_z, NULL, NULL))
				mvaddch(r, c, ' ');
}

static void ncurses_text(struct pane *p safe, struct pane *display safe,
			 wchar_t ch, int attr, int x, int y, int cursor)
{
	struct display_data *dd;
	cchar_t cc = {};
	int w=1, h=1;

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
	set_screen(dd->scr);
	if (cursor == 2) {
		dd->cursor.x = x;
		dd->cursor.y = y;
	}
	if (cursor == 1)
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
	{KEY_BACKSPACE, "Backspace"},
	{KEY_DL, "DelLine"},
	{KEY_IL, "InsLine"},
	{KEY_DC, "Del"},
	{KEY_IC, "Ins"},
	{KEY_ENTER, "Enter"},
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
	{'\r', "Return"},
	{'\t', "Tab"},
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
			strcpy(buf, n);
		else if (c < ' ')
			sprintf(buf, "C-Chr-%c", c+64);
		else
			sprintf(buf, "Chr-%lc", c);
	}

	record_key(p, buf);
	call("Keystroke", p, 0, NULL, buf);
}

static void do_send_mouse(struct pane *p safe, int x, int y, char *cmd safe)
{
	record_mouse(p, cmd, x, y);
	call("Mouse-event", p, 0, NULL, cmd, 0, NULL, NULL, NULL, x, y);
}

static void send_mouse(MEVENT *mev safe, struct pane *p safe)
{
	int x = mev->x;
	int y = mev->y;
	int b;
	char buf[100];

	/* MEVENT has lots of bits.  We want a few numbers */
	for (b = 1 ; b <= 4; b++) {
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

	while ((is_keycode = get_wch(&c)) != ERR) {
		if (c == KEY_MOUSE) {
			MEVENT mev;
			while (getmouse(&mev) != ERR)
				send_mouse(&mev, p);
		} else
			send_key(is_keycode, c, p);
	}
	return 1;
}

DEF_CMD(display_ncurses)
{
	struct pane *p = ncurses_init(ci->focus);
	if (p)
		return comm_call(ci->comm2, "callback:display", p);
	return -1;
}

void edlib_init(struct pane *ed safe)
{
	call_comm("global-set-command", ed, 0, NULL, "attach-display-ncurses",
		  &display_ncurses);
}
