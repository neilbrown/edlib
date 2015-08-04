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
#include "keymap.h"

struct display_data {
	SCREEN			*scr;
	struct event_base	*base;
	/* mode and next_mode are already shifted */
	int			mode, next_mode, numeric, extra;
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
static int ncurses_refresh(struct pane *p, struct pane *point_pane, int damage);
#define CMD(func, name) {func, name, ncurses_refresh}
#define DEF_CMD(comm, func, name) static struct command comm = CMD(func, name)


static void move_cursor(struct pane *p)
{
	int ox;
	int oy;

	while (p->parent)
		p = p->parent;

	ox = p->x;
	oy = p->y;
	while (p->focus) {
		p = p->focus;
		ox += p->x;
		oy += p->y;
		if (p->cx >= 0 && p->cy >= 0)
			move(oy+p->cy, ox+p->cx);
	}
}

static void ncurses_flush(int fd, short ev, void *P)
{
	struct pane *p = P;
	move_cursor(p);
	refresh();
}

static int nc_abort(struct command *c, struct cmd_info *ci)
{
	struct pane *p = ci->focus;
	struct display_data *dd = p->data;

	event_base_loopbreak(dd->base);
	return 1;
}
DEF_CMD(comm_abort, nc_abort, "abort");

static int nc_refresh(struct command *c, struct cmd_info *ci)
{
	struct pane *p = ci->focus;
	clear();
	pane_damaged(p,  DAMAGED_FORCE);
	pane_refresh(p);
	return 1;
}
DEF_CMD(comm_refresh, nc_refresh, "refresh");

static int ncurses_refresh(struct pane *p, struct pane *point_pane, int damage)
{
	struct display_data *dd = p->data;
	struct event *l;
	struct timeval tv;

	set_screen(dd->scr);

	if (damage & DAMAGED_SIZE) {
		getmaxyx(stdscr, p->h, p->w);
		p->h -= 1;
	}
	l = event_new(dd->base, -1, EV_TIMEOUT, ncurses_flush, p);
	event_priority_set(l, 0);
	tv.tv_sec = 0;
	tv.tv_usec = 0;
	event_add(l, &tv);
	return damage & DAMAGED_SIZE;
}

static void handle_winch(int sig, short ev, void *null);
struct pane *ncurses_init(struct event_base *base, struct map *map)
{
	WINDOW *w = initscr();
	struct pane *p;
	struct event *l;
	struct display_data *dd = malloc(sizeof(*dd));
	int c_x;

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
	dd->mode = 0;
	dd->next_mode = 0;
	dd->numeric = NO_NUMERIC;
	dd->extra = 0;

	current_screen = NULL;
	dd->base = base;
	p = pane_register(NULL, 0, ncurses_refresh, dd, NULL);
	p->keymap = map;

	key_register_mode("C-x", &c_x);
	key_add(map, K_MOD(c_x, 3), &comm_abort);
	key_add(map, 12, &comm_refresh);

	getmaxyx(stdscr, p->h, p->w); p->h-=1;

	l = event_new(base, 0, EV_READ|EV_PERSIST, input_handle, p);
	event_add(l, NULL);
	l = event_new(base, SIGWINCH, EV_SIGNAL|EV_PERSIST,
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

void pane_set_mode(struct pane *p, int mode, int transient)
{
	struct display_data *dd;
	while (p->parent)
		p = p->parent;
	if (p->refresh != ncurses_refresh)
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
	if (p->refresh != ncurses_refresh)
		return;
	dd = p->data;
	dd->numeric = numeric;
}

void pane_set_extra(struct pane *p, int extra)
{
	struct display_data *dd;
	while (p->parent)
		p = p->parent;
	if (p->refresh != ncurses_refresh)
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

	mvadd_wch(y, x, &cc);
}

static void send_key(int keytype, wint_t c, struct pane *p)
{
	struct display_data *dd = p->data;
	struct cmd_info ci = {0};

	if (keytype == KEY_CODE_YES) {
		switch(c) {
		case 01057: c = META(FUNC_KEY(KEY_PPAGE)); break;
		case 01051: c = META(FUNC_KEY(KEY_NPAGE)); break;
		case 01072: c = META(FUNC_KEY(KEY_UP)); break;
		case 01061: c = META(FUNC_KEY(KEY_DOWN)); break;
		case 01042: c = META(FUNC_KEY(KEY_LEFT)); break;
		case 01064: c = META(FUNC_KEY(KEY_RIGHT)); break;
		default:
			c = FUNC_KEY(c);
		}
	}

	ci.key = dd->mode | c;
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

static void do_send_mouse(struct pane *p, int x, int y, int cmd)
{
	struct display_data *dd = p->data;
	struct cmd_info ci = {0};

	ci.key = dd->mode | cmd;
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

	/* MEVENT has lots of bits.  We want a few numbers */
	for (b = 0 ; b < 4; b++) {
		mmask_t s = mev->bstate;
		if (BUTTON_PRESS(s, b+1))
			do_send_mouse(p, x, y, M_PRESS(b));
		if (BUTTON_RELEASE(s, b+1))
			do_send_mouse(p, x, y, M_RELEASE(b));
		if (BUTTON_CLICK(s, b+1))
			do_send_mouse(p, x, y, M_CLICK(b));
		else if (BUTTON_DOUBLE_CLICK(s, b+1))
			do_send_mouse(p, x, y, M_DCLICK(b));
		else if (BUTTON_TRIPLE_CLICK(s, b+1))
			do_send_mouse(p, x, y, M_TCLICK(b));
	}
	if (mev->bstate & REPORT_MOUSE_POSITION)
		do_send_mouse(p, x, y, M_MOVE);
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
