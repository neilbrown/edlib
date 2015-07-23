/*
 * Simple text rendering straight from a buffer.
 *
 * We have a starting mark and we render forward from
 * there wrapping as needed.  If we don't find point,
 * we then need to walk out from point until we reach
 * the size of viewport.
 * We keep 'top' and 'bot' as types marks so we get notified
 * when there is a change, and know to
 *
 */

#include <unistd.h>
#include <stdlib.h>
#include <wchar.h>
#include <curses.h>

#include "core.h"
#include "pane.h"
#include "view.h"
#include "keymap.h"

struct rt_data {
	struct view_data *v;
	struct mark	*top, *bot;
	int		ignore_point;
	int		target_x;
	struct command	type;
	int		typenum;
	struct pane	*pane;
};

static struct map *rt_map;

static int render_text_refresh(struct pane  *p, int damage);
#define	CMD(func, name) {func, name, render_text_refresh}
#define	DEF_CMD(comm, func, name) static struct command comm = CMD(func, name)

static int rt_fore(struct doc *d, struct pane *p, struct mark *m, int *x, int *y, int draw)
{
	wint_t ch = mark_next(d, m);
	if (ch == WEOF)
		return 0;
	if (ch == '\n') {
		*x = 0;
		*y += 1;
	} else {
		int w = 1;
		if (ch == '\t')
			w = 8 - (*x)%8;
		else if (ch < ' ')
			w = 2;
		else
			w = 1;
		if (*x + w >= p->w) {
			if (draw)
				pane_text(p, '\\', A_UNDERLINE, p->w-1, *y);
			*y += 1;
			*x = 0;
		}
		if (draw) {
			if (ch == '\t')
				;
			else if (ch < ' ') {
				pane_text(p, '^', A_UNDERLINE, *x, *y);
				pane_text(p, ch+'@', A_UNDERLINE, 1+*x, *y);
			} else {
				pane_text(p, ch, 0, *x, *y);
			}
		}
		*x += w;
	}
	return 1;
}

static int rt_back(struct doc *d, struct pane *p, struct mark *m, int *x, int *y)
{
	wint_t ch = mark_prev(d, m);
	if (ch == WEOF)
		return 0;
	if (ch == '\n') {
		*x = 0;
		*y -= 1;
	} else if (ch == '\t') {
		/* tricky err too large. */
		*x += 8;
	} else if (ch < ' ') {
		*x += 2;
	} else
		*x += 1;
	if (*x >= p->w-1) {
		*x = 0;
		*y -= 1;
	}
	return 1;
}

static struct mark *render(struct doc *d, struct point *pt, struct pane *p)
{
	struct mark *m;
	struct mark *last_vis;
	struct rt_data *rd = p->data;
	int x = 0, y = 0;
	wint_t ch;

	pane_clear(p, 0, 0, 0, 0, 0);

	m = mark_dup(rd->top, 0);
	last_vis = mark_dup(m, 0);

	p->cx = -1;
	p->cy = -1;

	ch = doc_prior(d, m);
	if (ch != WEOF && ch != '\n') {
		pane_text(p, '<', A_UNDERLINE, x, y);
		x += 1;
	}
	while (y < p->h) {
		mark_free(last_vis);
		last_vis = mark_dup(m, 0);
		if (mark_same(d, m, mark_of_point(pt))) {
			p->cx = x;
			p->cy = y;
		}
		if (rt_fore(d, p, m, &x, &y, 1) == 0)
			break;
	}
	mark_free(m);
	if (mark_ordered(mark_of_point(pt), rd->top))
		/* point is before mark, cannot possibly see cursor */
		p->cx = p->cy = -1;
	return last_vis;
}

static struct mark *find_pos(struct doc *d, struct pane *p, int px, int py)
{
	struct mark *m;
	struct rt_data *rd = p->data;
	int x = 0, y = 0;
	wint_t ch;

	m = mark_dup(rd->top, 1);

	ch = doc_prior(d, m);
	if (ch != WEOF && ch != '\n') {
		x += 1;
	}
	while (y < p->h) {
		if (y > py)
			break;
		if (y == py && x == px)
			return m;
		if (y == py && x > px)
			break;

		if (rt_fore(d, p, m, &x, &y, 0) == 0)
			break;
	}
	mark_prev(d, m);
	return m;
}

static struct mark *find_top(struct doc *d, struct point *pt, struct pane *p,
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
	struct rt_data *rt = p->data;
	struct mark *start, *end;
	int found_start = 0, found_end = 0;
	int sx=0, sy=0, ex=0, ey=0;
	wint_t ch;

	start = mark_at_point(pt, rt->typenum);
	end = mark_at_point(pt, rt->typenum);
	if (bot &&
	    (mark_ordered(start, bot) && ! mark_same(d, start, bot)))
		bot = NULL;
	if (top &&
	    (mark_ordered(top, end) && ! mark_same(d, top, end)))
		top = NULL;
	while (!((found_start && found_end) || ey-sy >= p->h-1)) {
		if (!found_start) {
			if (rt_back(d, p, start, &sx, &sy) == 0)
				found_start = 1;

			if (bot && mark_ordered(start, bot))
				found_end = 1;
		}
		if (!found_end) {
			if (rt_fore(d, p, end, &ex, &ey, 0) == 0)
				found_end = 1;

			if (top && mark_ordered(top, end))
				found_start = 1;
		}
	}
	/* Move 'start' to start of line if possible */
	ch = WEOF;
	while (sx < p->w-2 &&
	       (ch = mark_prev(d, start)) != WEOF &&
	       ch != '\n')
		sx++;
	if (ch == '\n')
		mark_next(d, start);
	/* I wonder if we should round off to a newline?? */
	mark_free(end);
	return start;
}

static int render_text_refresh(struct pane  *p, int damage)
{
	struct rt_data *rt = p->data;
	struct mark *end = NULL, *top;

	if (rt->top) {
		end = render(rt->v->doc, rt->v->point, p);
		if (rt->ignore_point || p->cx >= 0)
			/* Found the cursor! */
			goto found;
	}
	top = find_top(rt->v->doc, rt->v->point, p,
		       rt->top, end);
	mark_free(rt->top);
	mark_free(end);
	rt->top = top;
	end = render(rt->v->doc, rt->v->point, p);
found:
	mark_free(rt->bot);
	rt->bot = end;
	return 0;
}

static int render_text_notify(struct command *c, struct cmd_info *ci)
{
	struct rt_data *rt;
	if (ci->key != EV_REPLACE)
		return 0;
	rt = container_of(c, struct rt_data, type);
	if (ci->mark == rt->top)
		/* A change in the text between top and bot */
		pane_damaged(rt->pane, DAMAGED_CONTENT);
	return 0;
}

void render_text_attach(struct pane *p)
{
	struct rt_data *rt = malloc(sizeof(*rt));

	rt->v = p->data;
	rt->pane = p;
	rt->top = NULL;
	rt->bot = NULL;
	rt->ignore_point = 0;
	rt->target_x = -1;
	rt->type.func = render_text_notify;
	rt->type.name = "render_text_notify";
	rt->type.type = NULL;
	rt->typenum = doc_add_type(rt->v->doc, &rt->type);
	p->data = rt;
	p->refresh = render_text_refresh;
	p->keymap = rt_map;
}

static int render_text_move(struct command *c, struct cmd_info *ci)
{
	struct pane *p = ci->focus;
	int rpt = ci->repeat;
	struct rt_data *rt = p->data;
	int x = 0;
	int y = 0;

	if (!rt->top)
		return 0;
	if (rpt == INT_MAX)
		rpt = 1;
	if (ci->key == MV_VIEW_LARGE)
		rpt *= p->h - 2;
	rt->ignore_point = 1;
	if (rpt < 0) {
		while (rt_back(rt->v->doc, p, rt->top, &x, &y) && -y < 1-rpt)
			;
		if (-y >= 1-rpt)
			rt_fore(rt->v->doc, p, rt->top, &x, &y, 0);
	} else if (rpt > 0) {
		while (rt_fore(rt->v->doc, p, rt->top, &x, &y, 0) && y < rpt)
			;
	}
	pane_damaged(p, DAMAGED_CURSOR);
	return 1;
}
DEF_CMD(comm_move, render_text_move, "move-view");

static int render_text_follow_point(struct command *c, struct cmd_info *ci)
{
	struct pane *p = ci->focus;
	struct rt_data *rt = p->data;

	rt->ignore_point = 0;
	if (ci->key != MV_LINE)
		rt->target_x = -1;
	return 0;
}
DEF_CMD(comm_follow, render_text_follow_point, "follow-point");

static int render_text_set_cursor(struct command *c, struct cmd_info *ci)
{
	struct pane *p = ci->focus;
	struct rt_data *rt = p->data;
	struct mark *m;

	m = find_pos(rt->v->doc, p, ci->x, ci->y);
	point_to_mark(rt->v->doc, rt->v->point, m);
	mark_free(m);
	pane_focus(p);
	return 1;
}
DEF_CMD(comm_cursor, render_text_set_cursor, "set-cursor");

static int render_text_move_line(struct command *c, struct cmd_info *ci)
{
	struct pane *p = ci->focus;
	/* MV_EOL repeatedly, then move to match cursor */
	struct rt_data *rt = p->data;
	struct cmd_info ci2 = {0};
	struct mark *m;
	int ret = 0;
	int x, y;
	int target_x;

	if (rt->target_x < 0)
		rt->target_x = p->cx;
	target_x = rt->target_x;

	ci2.focus = ci->focus;
	ci2.key = MV_EOL;
	if (ci->repeat < 0)
		ci2.repeat = ci->repeat-1;
	else
		ci2.repeat = ci->repeat;
	m = mark_of_point(rt->v->point);
	ci2.mark = m;
	ret = key_handle_focus(&ci2);

	if (!ret)
		return 0;
	rt->target_x = target_x; // MV_EOL might have changed it
	if (ci->repeat > 0)
		mark_next(rt->v->doc, m);

	if (target_x == 0)
		return 1;
	x = 0; y = 0;
	while (rt_fore(rt->v->doc, p, m, &x, &y, 0) == 1) {
		if (y > 0 || x > target_x) {
			/* too far */
			mark_prev(rt->v->doc, m);
			break;
		}
	}
	pane_damaged(p, DAMAGED_CURSOR);
	return 1;
}
DEF_CMD(comm_line, render_text_move_line, "move-line");

void render_text_register(struct map *m)
{
	rt_map = key_alloc();

	key_add(rt_map, MV_VIEW_SMALL, &comm_move);
	key_add(rt_map, MV_VIEW_LARGE, &comm_move);
	key_add(rt_map, MV_CURSOR_XY, &comm_cursor);
	key_add(rt_map, M_CLICK(0), &comm_cursor);
	key_add(rt_map, M_PRESS(0), &comm_cursor);
	key_add(rt_map, MV_LINE, &comm_line);

	key_add_range(rt_map, MV_CHAR, MV_LINE-1, &comm_follow);
	key_add_range(rt_map, MV_LINE+1, MV_FILE, &comm_follow);
	key_add(rt_map, EV_REPLACE, &comm_follow);
}
