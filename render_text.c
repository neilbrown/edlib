/*
 * Simple text rendering straight from a buffer.
 *
 * We have a starting mark and we render forward from
 * there wrapping as needed.  If we don't find point,
 * we then need to walk out from point until we reach
 * the size of viewport.
 *
 */

#include <unistd.h>
#include <stdlib.h>
#include <wchar.h>
#include <curses.h>

#include "list.h"
#include "text.h"
#include "mark.h"
#include "pane.h"
#include "view.h"
#include "keymap.h"

struct rt_data {
	struct view_data *v;
	struct mark	*top;
	int		ignore_point;
};

static struct map *rt_map;

static int rt_fore(struct text *t, struct pane *p, struct mark *m, int *x, int *y, int draw)
{
	wint_t ch = mark_next(t, m);
	if (ch == WEOF)
		return 0;
	if (ch == '\n') {
		*x = 0;
		*y += 1;
	} else if (ch == '\t') {
		*x += 8;
		*x -= (*x)%8;
	} else{
		if (*x > p->w - 1) {
			if (draw)
				pane_text(p, '\\', A_UNDERLINE, *x, *y);
			*y += 1;
			*x = 0;
		}
		if (ch < ' ') {
			if (draw) {
				pane_text(p, '^', A_UNDERLINE, *x, *y);
				pane_text(p, ch+'@', A_UNDERLINE, 1+*x, *y);
			}
			*x += 2;
		} else {
			if (draw)
				pane_text(p, ch, 0, *x, *y);
			*x += 1;
		}
	}
	return 1;
}

static int rt_back(struct text *t, struct pane *p, struct mark *m, int *x, int *y)
{
	wint_t ch = mark_prev(t, m);
	if (ch == WEOF)
		return 0;
	if (ch == '\n') {
		*x = 0;
		*y -= 1;
	} else if (ch == '\t') {
		/* tricky... */
		*x += 6;
	} else if (ch < ' ') {
		*x += 2;
	} else
		*x += 1;
	if (*x > p->w) {
		*x = 0;
		*y -= 1;
	}
	return 1;
}

static struct mark *render(struct text *t, struct point *pt, struct pane *p)
{
	struct mark *m;
	struct mark *last_vis;
	struct rt_data *rd = p->data;
	int x = 0, y = 0;
	wint_t ch;

	pane_clear(p, 0, 0, 0, 0, 0);

	m = mark_dup(rd->top, 1);
	last_vis = m;

	p->cx = -1;
	p->cy = -1;

	ch = mark_prior(t, m);
	if (ch != WEOF && ch != '\n') {
		pane_text(p, '<', A_UNDERLINE, x, y);
		x += 1;
	}
	while (y < p->h) {
		last_vis = m;
		if (mark_same(m, mark_of_point(pt))) {
			p->cx = x;
			p->cy = y;
		}
		if (rt_fore(t, p, m, &x, &y, 1) == 0)
			break;
	}
	return last_vis;
}

static struct mark *find_pos(struct text *t, struct pane *p, int px, int py)
{
	struct mark *m;
	struct rt_data *rd = p->data;
	int x = 0, y = 0;
	wint_t ch;

	m = mark_dup(rd->top, 1);

	ch = mark_prior(t, m);
	if (ch != WEOF && ch != '\n') {
		x += 1;
	}
	while (y < p->h) {
		if (y > py)
			return m;
		if (y == py && x >= px)
			return m;

		if (rt_fore(t, p, m, &x, &y, 0) == 0)
			break;
	}
	mark_prev(t, m);
	return m;
}

static struct mark *find_top(struct text *t, struct point *pt, struct pane *p,
			     struct mark *top, struct mark *bot)
{
	/* top and bot might be NULL, else they record what is currently
	 * in the pane.
	 * We walk outwards from pt until we reach extremes of buffer,
	 * or cross top (from above) or bot (from below).
	 * When end hits EOF or start crosses bot, end stops moving.
	 * When start hits SOF or end crosses top, start stops moving.
	 * When number of lines reaches height of pane, both top moving.
	 * At this point, 'start' is the new 'top'.
	 */
	struct mark *start, *end;
	int found_start = 0, found_end = 0;
	int sx=0, sy=0, ex=0, ey=0;
	wint_t ch;

	start = mark_at_point(pt, MARK_UNGROUPED);
	end = mark_at_point(pt, MARK_UNGROUPED);
	if (bot &&
	    (mark_ordered(start, bot) && ! mark_same(start, bot)))
		bot = NULL;
	if (top &&
	    (mark_ordered(top, end) && ! mark_same(top, end)))
		top = NULL;
	while (!((found_start && found_end) || ey-sy >= p->h-1)) {
		if (!found_start) {
			if (rt_back(t, p, start, &sx, &sy) == 0)
				found_start = 1;

			if (bot && mark_ordered(start, bot))
				found_end = 1;
		}
		if (!found_end) {
			if (rt_fore(t, p, end, &ex, &ey, 0) == 0)
				found_end = 1;

			if (top && mark_ordered(top, end))
				found_start = 1;
		}
	}
	/* Move 'start' to start of line if possible */
	ch = WEOF;
	while (sx < p->w-2 &&
	       (ch = mark_prev(t, start)) != WEOF &&
	       ch != '\n')
		sx++;
	if (ch == '\n')
		mark_next(t, start);
	/* I wonder if we should round off to a newline?? */
	mark_delete(end);
	return start;
}

int render_text_refresh(struct pane  *p, int damage)
{
	struct rt_data *rt = p->data;
	struct mark *end = NULL, *top;

	if (rt->top) {
		end = render(rt->v->text, rt->v->point, p);
		if (rt->ignore_point || p->cx >= 0) {
			/* Found the cursor! */
			mark_delete(end);
			return 1;
		}
	}
	top = find_top(rt->v->text, rt->v->point, p,
		       rt->top, end);
	if (rt->top)
		mark_delete(rt->top);
	if (end)
		mark_delete(end);
	rt->top = top;
	end = render(rt->v->text, rt->v->point, p);
	mark_delete(end);
	return 1;
}

void render_text_attach(struct pane *p)
{
	struct rt_data *rt = malloc(sizeof(*rt));

	rt->v = p->data;
	rt->top = NULL;
	rt->ignore_point = 0;
	p->data = rt;
	p->refresh = render_text_refresh;
	p->keymap = rt_map;
}

static int render_text_move(struct command *c, int key, struct cmd_info *ci)
{
	struct pane *p = ci->focus;
	int rpt = ci->repeat;
	struct rt_data *rt;
	int x = 0;
	int y = 0;

	while (p && p->refresh != render_text_refresh)
		p = p->parent;
	if (!p)
		return 0;
	rt = p->data;
	if (!rt->top)
		return 0;
	if (rpt == INT_MAX)
		rpt = 1;
	if (key == MV_VIEW_LARGE)
		rpt *= p->h - 2;
	rt->ignore_point = 1;
	if (rpt < 0) {
		while (rt_back(rt->v->text, p, rt->top, &x, &y) && -y < 1-rpt)
			;
		if (-y >= 1-rpt)
			rt_fore(rt->v->text, p, rt->top, &x, &y, 0);
	} else if (rpt > 0) {
		while (rt_fore(rt->v->text, p, rt->top, &x, &y, 0) && y < rpt)
			;
	}
	pane_focus(p);
	return 1;
}
static struct command comm_move = {render_text_move, "move-view"};

static int render_text_follow_point(struct command *c, int key, struct cmd_info *ci)
{
	struct pane *p = ci->focus;
	struct rt_data *rt;
	while (p && p->refresh != render_text_refresh)
		p = p->parent;
	if (!p)
		return 0;
	rt = p->data;
	rt->ignore_point = 0;
	return 0;
}
static struct command comm_follow = {render_text_follow_point, "follow-point"};

static int render_text_set_cursor(struct command *c, int key, struct cmd_info *ci)
{
	struct pane *p = ci->focus;
	struct rt_data *rt;
	struct mark *m;
	while (p && p->refresh != render_text_refresh) {
		ci->x += p->x;
		ci->y += p->y;
		p = p->parent;
	}
	if (!p)
		return 0;
	rt = p->data;
	m = find_pos(rt->v->text, p, ci->x, ci->y);
	point_to_mark(rt->v->text, rt->v->point, m);
	mark_delete(m); free(m);
	pane_focus(p);
	return 1;
}
static struct command comm_cursor = {render_text_set_cursor, "set-cursor"};

void render_text_register(struct map *m)
{
	rt_map = key_alloc();

	key_add(rt_map, MV_VIEW_SMALL, &comm_move);
	key_add(rt_map, MV_VIEW_LARGE, &comm_move);
	key_add(rt_map, MV_CURSOR_XY, &comm_cursor);
	key_add(rt_map, M_CLICK(0), &comm_cursor);
	key_add(rt_map, M_PRESS(0), &comm_cursor);

	key_add_range(rt_map, MV_CHAR, MV_FILE, &comm_follow);
	key_add(rt_map, EV_REPLACE, &comm_follow);
}
