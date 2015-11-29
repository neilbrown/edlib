/*
 * Copyright Neil Brown <neil@brown.name> 2015
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

#define _XOPEN_SOURCE
#define _XOPEN_SOURCE_EXTENDED

#include <stdlib.h>
#include <curses.h>
#include <string.h>
#include <locale.h>
#include <ctype.h>
#include <event.h>
#include <signal.h>
#include <sys/ioctl.h>

#include "core.h"

struct display_data {
	struct display		dpy;
	SCREEN			*scr;
};

static SCREEN *current_screen;
static void ncurses_clear(struct pane *p, int attr, int x, int y, int w, int h);
static void ncurses_text(struct pane *p, wchar_t ch, int attr, int x, int y);

static void set_screen(SCREEN *scr)
{
	if (scr == current_screen)
		return;
	set_term(scr);
	current_screen = scr;
}

static void input_handle(int fd, short ev, void *P);

static void move_cursor(struct pane *p)
{
	int ox;
	int oy;

	p = pane_with_cursor(p, &ox, &oy);

	if (p) {
		int cx = p->cx;
		int cy = p->cy;
		if (cx >= p->w)
			cx = p->w - 1;
		if (cy >= p->h)
			cy = p->h - 1;
		move(oy+cy, ox+cx);
	}
}

static void ncurses_flush(int fd, short ev, void *P)
{
	struct pane *p = P;
	move_cursor(p);
	refresh();
}

DEF_CMD(nc_misc)
{
	struct pane *p = ci->home;
	struct editor *ed = pane2ed(p);

	if (strcmp(ci->str, "exit") == 0)
		event_base_loopbreak(ed->base);
	else if (strcmp(ci->str, "refresh") == 0) {
		clear();
		pane_damaged(p,  DAMAGED_SIZE);
		pane_refresh(p);
	} else
		return 0;
	return 1;
}

static void ncurses_end(void)
{
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
		}
		a = c;
	}
	return attr;
}

DEF_CMD(ncurses_handle)
{
	struct pane *p = ci->home;
	int damage = ci->extra;
	struct display_data *dd = p->data;
	struct event *l;
	struct timeval tv;

	if (strcmp(ci->key, "Misc") == 0)
		return nc_misc.func(ci);

	if (strcmp(ci->key, "Close") == 0) {
		ncurses_end();
		return 1;
	}
	if (strcmp(ci->key, "pane-clear") == 0) {
		int attr = cvt_attrs(ci->str2);
		ncurses_clear(ci->focus, attr, 0, 0, 0, 0);
		return 1;
	}
	if (strcmp(ci->key, "pane-text") == 0) {
		int attr = cvt_attrs(ci->str2);
		ncurses_text(ci->focus, ci->extra, attr, ci->x, ci->y);
		return 1;
	}
	if (strcmp(ci->key, "Refresh") == 0) {
		struct editor *ed = pane2ed(p);
		set_screen(dd->scr);

		if (damage & DAMAGED_SIZE) {
			getmaxyx(stdscr, p->h, p->w);
			p->h -= 1;
		}
		l = event_new(ed->base, -1, EV_TIMEOUT, ncurses_flush, p);
		event_priority_set(l, 0);
		tv.tv_sec = 0;
		tv.tv_usec = 0;
		event_add(l, &tv);
		return 1;
	}
	return 0;
}

static void handle_winch(int sig, short ev, void *null);
static struct pane *ncurses_init(struct editor *ed)
{
	WINDOW *w = initscr();
	struct pane *p;
	struct event *l;
	struct display_data *dd = malloc(sizeof(*dd));

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
	dd->dpy.mode = "";
	dd->dpy.next_mode = "";
	dd->dpy.numeric = NO_NUMERIC;
	dd->dpy.extra = 0;

	current_screen = NULL;
	p = pane_register(&ed->root, 0, &ncurses_handle, dd, NULL);

	getmaxyx(stdscr, p->h, p->w); p->h-=1;

	l = event_new(ed->base, 0, EV_READ|EV_PERSIST, input_handle, p);
	event_add(l, NULL);
	l = event_new(ed->base, SIGWINCH, EV_SIGNAL|EV_PERSIST,
		      handle_winch, p);
	event_add(l, NULL);
	pane_damaged(p, DAMAGED_SIZE);
	return p;
}

static void handle_winch(int sig, short ev, void *vpane)
{
	struct pane *p = vpane;
	struct winsize size;
	ioctl(1, TIOCGWINSZ, &size);
	resize_term(size.ws_row, size.ws_col);

	clear();
	pane_damaged(p, DAMAGED_SIZE);
	pane_refresh(p);
}

static void ncurses_clear(struct pane *p, int attr, int x, int y, int w, int h)
{
	int r, c;
	struct display_data *dd;
	int z = p->z;
	int w0, h0;

	if (w == 0)
		w = p->w - x;
	if (h == 0)
		h = p->h - y;
	p = pane_to_root(p, &x, &y, &z, &w, &h);
	w0 = w; h0 = h;
	if (pane_masked(p, x, y, z, &w0, &h0))
		w0 = h0 = 0;

	dd = p->data;
	set_screen(dd->scr);
	attrset(attr);
	for (r = y; r < y+h; r++)
		for (c = x; c < x+w; c++)
			if ((r < y+h0 && c < x+w0) || !pane_masked(p, c, r, z, NULL, NULL))
				mvaddch(r, c, ' ');
}

static void ncurses_text(struct pane *p, wchar_t ch, int attr, int x, int y)
{
	struct display_data *dd;
	cchar_t cc = {0};
	int w=1, h=1;
	int z = p->z;

	if (x < 0 || y < 0)
		return;

	p = pane_to_root(p, &x, &y, &z, &w, &h);
	if (w < 1 || h < 1)
		return;

	if (pane_masked(p, x, y, z, NULL, NULL))
		return;

	dd = p->data;
	set_screen(dd->scr);
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

static char *find_name (struct namelist *l, wint_t c)
{
	int i;
	for (i = 0; l[i].name; i++)
		if (l[i].key == c)
			return l[i].name;
	return NULL;
}

static void send_key(int keytype, wint_t c, struct pane *p)
{
	struct display_data *dd = p->data;
	struct cmd_info ci = {0};
	char *k, *n;
	char buf[100];/* FIXME */

	strcpy(buf, dd->dpy.mode);
	k = buf + strlen(buf);
	if (keytype == KEY_CODE_YES) {
		n = find_name(key_names, c);
		if (!n)
			sprintf(k, "Ncurs-%o", c);
		else
			strcpy(k, n);
	} else {
		n = find_name(char_names, c);
		if (n)
			strcpy(k, n);
		else if (c < ' ')
			sprintf(k, "C-Chr-%c", c+64);
		else
			sprintf(k, "Chr-%lc", c);
	}

	ci.key = buf;
	ci.focus = p;
	ci.numeric = dd->dpy.numeric;
	ci.extra = dd->dpy.extra;
	dd->dpy.mode = dd->dpy.next_mode;
	dd->dpy.numeric = NO_NUMERIC;
	dd->dpy.extra = 0;
	key_handle_focus_point(&ci);
}

static void do_send_mouse(struct pane *p, int x, int y, char *cmd)
{
	struct display_data *dd = p->data;
	struct cmd_info ci = {0};
	char buf[100];/* FIXME */

	ci.key = strcat(strcpy(buf, dd->dpy.mode), cmd);
	ci.focus = p;
	ci.numeric = dd->dpy.numeric;
	ci.extra = dd->dpy.extra;
	ci.x = x;
	ci.y = y;
	dd->dpy.mode = dd->dpy.next_mode;
	dd->dpy.numeric = NO_NUMERIC;
	dd->dpy.extra = 0;
	key_handle_xy_point(&ci);
}

static void send_mouse(MEVENT *mev, struct pane *p)
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

static void input_handle(int fd, short ev, void *P)
{
	struct pane *p = P;
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
	pane_refresh(p);
}

DEF_CMD(comm_ncurses)
{
	struct pane *p = ncurses_init(pane2ed(ci->home));
	ci->focus = p;
	return 1;
}

void edlib_init(struct editor *ed)
{
	key_add(ed->commands, "display-ncurses", &comm_ncurses);
}
