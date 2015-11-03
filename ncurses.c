/*
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
#include "pane.h"
#include "tile.h"

struct display_data {
	struct display		dpy;
	SCREEN			*scr;
	char			*mode, *next_mode;
	int			numeric, extra;
};

static SCREEN *current_screen;

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

static int nc_misc(struct command *c, struct cmd_info *ci)
{
	struct pane *p = ci->focus;
	struct display_data *dd = p->data;

	if (strcmp(ci->str, "exit") == 0)
		event_base_loopbreak(dd->dpy.ed->base);
	else if (strcmp(ci->str, "refresh") == 0) {
		clear();
		pane_damaged(p,  DAMAGED_FORCE);
		pane_refresh(p);
	} else
		return 0;
	return 1;
}
DEF_CMD(comm_misc, nc_misc, "misc");

static int do_ncurses_refresh(struct command *c, struct cmd_info *ci)
{
	struct pane *p = ci->focus;
	int damage = ci->extra;
	struct display_data *dd = p->data;
	struct event *l;
	struct timeval tv;

	if (strcmp(ci->key, "Close") == 0) {
		/* FIXME */
	}
	if (strcmp(ci->key, "Refresh") != 0)
		return 0;

	if (p->focus == NULL) {
		/* Choose child with largest z */
		struct pane *c;
		list_for_each_entry(c, &p->children, siblings)
			if (p->focus == NULL ||
			    c->z > p->focus->z)
				p->focus = c;
	}

	set_screen(dd->scr);

	if (damage & DAMAGED_SIZE) {
		getmaxyx(stdscr, p->h, p->w);
		p->h -= 1;
	}
	l = event_new(dd->dpy.ed->base, -1, EV_TIMEOUT, ncurses_flush, p);
	event_priority_set(l, 0);
	tv.tv_sec = 0;
	tv.tv_usec = 0;
	event_add(l, &tv);
	ci->extra &= DAMAGED_SIZE;
	return 1;
}
DEF_CMD(ncurses_refresh, do_ncurses_refresh, "ncurses-refresh");

static void handle_winch(int sig, short ev, void *null);
struct pane *ncurses_init(struct editor *ed, struct map *map)
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

	dd->dpy.ed = ed;
	dd->scr = NULL;
	dd->mode = 0;
	dd->next_mode = 0;
	dd->numeric = NO_NUMERIC;
	dd->extra = 0;

	current_screen = NULL;
	p = pane_register(NULL, 0, &ncurses_refresh, dd, NULL);
	p->keymap = map;

	key_add(map, "Misc", &comm_misc);

	getmaxyx(stdscr, p->h, p->w); p->h-=1;

	l = event_new(ed->base, 0, EV_READ|EV_PERSIST, input_handle, p);
	event_add(l, NULL);
	l = event_new(ed->base, SIGWINCH, EV_SIGNAL|EV_PERSIST,
		      handle_winch, p);
	event_add(l, NULL);
	pane_damaged(p, DAMAGED_SIZE | DAMAGED_FORCE);
	return p;
}

void ncurses_end(void)
{
	nl();
	endwin();
}

static void handle_winch(int sig, short ev, void *vpane)
{
	struct pane *p = vpane;
	struct winsize size;
	ioctl(1, TIOCGWINSZ, &size);
	resize_term(size.ws_row, size.ws_col);

	clear();
	pane_damaged(p, DAMAGED_SIZE|DAMAGED_FORCE);
	pane_refresh(p);
}

void pane_set_mode(struct pane *p, char *mode, int transient)
{
	struct display_data *dd;
	while (p->parent)
		p = p->parent;
	if (p->refresh != &ncurses_refresh)
		return;
	dd = p->data;
	dd->mode = mode;
	if (!transient)
		dd->next_mode = mode;
}

void pane_set_numeric(struct pane *p, int numeric)
{
	struct display_data *dd;
	while (p->parent)
		p = p->parent;
	if (p->refresh != &ncurses_refresh)
		return;
	dd = p->data;
	dd->numeric = numeric;
}

void pane_set_extra(struct pane *p, int extra)
{
	struct display_data *dd;
	while (p->parent)
		p = p->parent;
	if (p->refresh != &ncurses_refresh)
		return;
	dd = p->data;
	dd->extra = extra;
}

void pane_clear(struct pane *p, int attr, int x, int y, int w, int h)
{
	int r, c;
	struct display_data *dd;
	int z = p->z;
	int w0, h0;

	if (w == 0)
		w = p->w - x;
	if (h == 0)
		h = p->h - y;
	p = pane_to_root(p, &x, &y, &w, &h);
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

void pane_text(struct pane *p, wchar_t ch, int attr, int x, int y)
{
	struct display_data *dd;
	cchar_t cc;
	int w=1, h=1;
	int z = p->z;
	p = pane_to_root(p, &x, &y, &w, &h);
	if (w < 1 || h < 1)
		return;

	if (pane_masked(p, x, y, z, NULL, NULL))
		return;
	dd = p->data;
	set_screen(dd->scr);
	cc.attr = attr;
	cc.chars[0] = ch;
	cc.chars[1] = 0;
	#ifdef NCURSES_EXT_COLORS
	cc.ext_color = 0;
	#endif

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

	strcpy(buf, dd->mode);
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
			sprintf(k, "Chr-%c", c);
	}

	ci.key = buf;
	ci.focus = p;
	ci.numeric = dd->numeric;
	ci.extra = dd->extra;
	ci.x = ci.y = -1;
	// FIXME find doc
	dd->mode = dd->next_mode;
	dd->numeric = NO_NUMERIC;
	dd->extra = 0;
	key_handle_focus(&ci);
}

static void do_send_mouse(struct pane *p, int x, int y, char *cmd)
{
	struct display_data *dd = p->data;
	struct cmd_info ci = {0};
	char buf[100];/* FIXME */

	ci.key = strcat(strcpy(buf, dd->mode), cmd);
	ci.focus = p;
	ci.numeric = dd->numeric;
	ci.extra = dd->extra;
	ci.x = x;
	ci.y = y;
	// FIXME find doc
	dd->mode = dd->next_mode;
	dd->numeric = NO_NUMERIC;
	dd->extra = 0;
	key_handle_xy(&ci);
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
		if (BUTTON_RELEASE(s, b))
			action = "Release-%d";
		if (BUTTON_CLICK(s, b))
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
