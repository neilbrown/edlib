/*
 * Copyright Neil Brown <neil@brown.name> 2015
 * May be distributed under terms of GPLv2 - see file:COPYING
 *
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
#include <string.h>

#include "core.h"
#include "extras.h"

struct rt_data {
	struct mark	*top, *bot;
	int		top_sol; /* true when 'top' is at a start-of-line */
	int		ignore_point;
	int		target_x;
	struct command	type;
	int		typenum;
	struct pane	*pane;
	int		prefix_len;
};

static struct map *rt_map;
static void render_text_attach(struct pane *p, struct point **pt);

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

static struct mark *render(struct point **ptp, struct pane *p)
{
	struct mark *m;
	struct mark *last_vis;
	struct rt_data *rd = p->data;
	struct doc *d = (*ptp)->doc;
	int x = 0, y = 0;
	wint_t ch;
	char *prefix;

	pane_clear(p, 0, 0, 0, 0, 0);

	prefix = doc_attr(d, NULL, 0, "prefix");
	if (prefix) {
		char *s = prefix;
		while (*s) {
			pane_text(p, *s, A_BOLD, x, y);
			x += 1;
			s += 1;
		}
	}
	rd->prefix_len = x;

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
		if (mark_same(d, m, mark_of_point(*ptp))) {
			p->cx = x;
			p->cy = y;
		}
		if (rt_fore(d, p, m, &x, &y, 1) == 0)
			break;
	}
	mark_free(m);
	if (mark_ordered(mark_of_point(*ptp), rd->top))
		/* point is before mark, cannot possibly see cursor */
		p->cx = p->cy = -1;
	while (mark_ordered(last_vis, mark_of_point(*ptp)) &&
	       mark_same(d, last_vis, mark_of_point(*ptp)))
		/* point is at end of visible region - need to include it */
		mark_forward_over(last_vis, doc_next_mark_all(d, last_vis));

	return last_vis;
}

static struct mark *find_pos(struct doc *d, struct pane *p, int px, int py)
{
	struct mark *m;
	struct rt_data *rd = p->data;
	int x = 0, y = 0;
	wint_t ch;

	m = mark_dup(rd->top, 1);

	x += rd->prefix_len;

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

static struct mark *find_top(struct point **ptp, struct pane *p,
			     struct mark *top, struct mark *bot)
{
	/* top and bot might be NULL, else they record what is currently
	 * in the pane.
	 * We walk outwards from ptp until we reach extremes of buffer,
	 * or cross top (from above) or bot (from below).
	 * When end hits EOF or start crosses bot, end stops moving.
	 * When start hits SOF or end crosses top, start stops moving.
	 * When number of lines reaches height of pane, both stop moving.
	 * At this point, 'start' is the new 'top'.
	 */
	struct rt_data *rt = p->data;
	struct mark *start, *end;
	struct doc *d = (*ptp)->doc;
	int found_start = 0, found_end = 0;
	int sx=0, sy=0, ex=0, ey=0;
	wint_t ch;

	start = mark_at_point(*ptp, rt->typenum);
	end = mark_at_point(*ptp, rt->typenum);
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
	/* FIXME this is a bit simplistic and may not handle short windows
	 * or long lines well.
	 */
	if (ey > 0 || sy <= 1) {
		/* Move 'start' to start of line if possible */
		while (sx < p->w-2 &&
		       (ch = mark_prev(d, start)) != WEOF &&
		       ch != '\n')
			sx++;
		if (ch == '\n')
			mark_next(d, start);
	} else {
		/* cursor is very near bottom, move 'start' to end of line */
		while (sx < p->w*2 &&
		       (ch = mark_next(d, start)) != WEOF &&
		       ch != '\n')
			sx++;
	}
	/* I wonder if we should round off to a newline?? */
	mark_free(end);
	return start;
}

static int do_render_text_handle(struct command *c, struct cmd_info *ci)
{
	struct pane *p = ci->home;
	struct rt_data *rt = p->data;
	struct mark *end = NULL, *top;
	struct doc *d;
	int ret;

	ret = key_lookup(rt_map, ci);
	if (ret)
		return ret;

	if (strcmp(ci->key, "Close") == 0) {
		struct pane *p = rt->pane;
		d = (*ci->pointp)->doc;
		mark_free(rt->top);
		mark_free(rt->bot);
		rt->pane = NULL;
		doc_del_view(d, &rt->type);
		p->data = NULL;
		p->handle = NULL;
		free(rt);
		return 0;
	}
	if (strcmp(ci->key, "Clone") == 0) {
		struct pane *parent = ci->focus;

		render_text_attach(parent, NULL);
		if (p->focus)
			return pane_clone(p->focus, parent->focus);
		return 1;
	}
	if (strcmp(ci->key, "Refresh") != 0)
		return 0;

	pane_check_size(p);

	d = (*ci->pointp)->doc;

	if (p->focus == NULL && !list_empty(&p->children))
		p->focus = list_first_entry(&p->children, struct pane, siblings);
	if (rt->top && rt->top_sol &&
	    doc_prior(d, rt->top) != '\n' && doc_prior(d, rt->top) != WEOF) {
		top = find_top(ci->pointp, p, rt->top, end);
		mark_free(rt->top);
		rt->top = top;
	}
	if (rt->top) {
		end = render(ci->pointp, p);
		if (rt->ignore_point || p->cx >= 0)
			/* Found the cursor! */
			goto found;
	}
	top = find_top(ci->pointp, p, rt->top, end);
	mark_free(rt->top);
	mark_free(end);
	rt->top = top;
	rt->top_sol = (doc_prior(d, rt->top) == '\n' ||
		       doc_prior(d, rt->top) == WEOF);
	end = render(ci->pointp, p);
found:
	mark_free(rt->bot);
	rt->bot = end;
	return 0;
}
DEF_CMD(render_text_handle, do_render_text_handle);

static int render_text_notify(struct command *c, struct cmd_info *ci)
{
	struct rt_data *rt = container_of(c, struct rt_data, type);

	if (strcmp(ci->key, "Replace") == 0) {
		if (ci->mark == rt->top)
			/* A change in the text between top and bot */
			pane_damaged(rt->pane, DAMAGED_CONTENT);
		return 0;
	}
	if (strcmp(ci->key, "Release") == 0) {
		if (rt->pane)
			pane_close(rt->pane);
		return 1;
	}
	return 0;
}

static int render_text_move(struct command *c, struct cmd_info *ci)
{
	struct pane *p = ci->home;
	int rpt = RPT_NUM(ci);
	struct rt_data *rt = p->data;
	struct point *pt = *ci->pointp;
	int x = 0;
	int y = 0;

	if (!rt->top)
		return 0;
	if (strcmp(ci->key, "Move-View-Large") == 0)
		rpt *= p->h - 2;
	rt->ignore_point = 1;
	if (rpt < 0) {
		while (rt_back(pt->doc, p, rt->top, &x, &y) && -y < 1-rpt)
			;
		if (-y >= 1-rpt)
			rt_fore(pt->doc, p, rt->top, &x, &y, 0);
	} else if (rpt > 0) {
		while (rt_fore(pt->doc, p, rt->top, &x, &y, 0) && y < rpt)
			;
	}
	pane_damaged(p, DAMAGED_CURSOR);
	return 1;
}
DEF_CMD(comm_move, render_text_move);

static int render_text_follow_point(struct command *c, struct cmd_info *ci)
{
	struct pane *p = ci->home;
	struct rt_data *rt = p->data;

	if (rt->ignore_point) {
		pane_damaged(p, DAMAGED_CURSOR);
		rt->ignore_point = 0;

		if (strcmp(ci->key, "Move-Line") != 0)
			rt->target_x = -1;
	}
	return 0;
}
DEF_CMD(comm_follow, render_text_follow_point);

static int render_text_set_cursor(struct command *c, struct cmd_info *ci)
{
	struct pane *p = ci->home;
	struct point *pt = *ci->pointp;
	struct mark *m;

	m = find_pos(pt->doc, p, ci->x, ci->y);
	point_to_mark(pt, m);
	mark_free(m);
	pane_focus(p);
	return 1;
}
DEF_CMD(comm_cursor, render_text_set_cursor);

static int render_text_move_line(struct command *c, struct cmd_info *ci)
{
	struct pane *p = ci->home;
	/* MV_EOL repeatedly, then move to match cursor */
	struct rt_data *rt = p->data;
	struct point *pt = *ci->pointp;
	struct cmd_info ci2 = {0};
	struct mark *m;
	int ret = 0;
	int x, y;
	int target_x;

	if (rt->target_x < 0)
		rt->target_x = p->cx;
	target_x = rt->target_x;

	ci2.focus = ci->focus;
	ci2.key = "Move-EOL";
	ci2.numeric = RPT_NUM(ci);
	if (ci2.numeric < 0)
		ci2.numeric -= 1;
	m = mark_of_point(pt);
	ci2.mark = m;
	ci2.pointp = ci->pointp;
	ret = key_handle_focus(&ci2);

	if (!ret)
		return 0;
	rt->target_x = target_x; // MV_EOL might have changed it
	if (RPT_NUM(ci) > 0)
		mark_next(pt->doc, m);

	if (target_x == 0)
		return 1;
	x = 0; y = 0;
	while (rt_fore(pt->doc, p, m, &x, &y, 0) == 1) {
		if (y > 0 || x > target_x) {
			/* too far */
			mark_prev(pt->doc, m);
			break;
		}
	}
	pane_damaged(p, DAMAGED_CURSOR);
	return 1;
}
DEF_CMD(comm_line, render_text_move_line);

static void render_text_register_map(void)
{
	rt_map = key_alloc();

	key_add_range(rt_map, "Move-", "Move-\377", &comm_follow);
	key_add(rt_map, "Move-View-Small", &comm_move);
	key_add(rt_map, "Move-View-Large", &comm_move);
	key_add(rt_map, "Move-CursorXY", &comm_cursor);
	key_add(rt_map, "Click-1", &comm_cursor);
	key_add(rt_map, "Press-1", &comm_cursor);
	key_add(rt_map, "Move-Line", &comm_line);

	key_add(rt_map, "Replace", &comm_follow);
}

static void render_text_attach(struct pane *parent, struct point **ptp)
{
	struct rt_data *rt = malloc(sizeof(*rt));
	struct pane *p;

	if (!ptp)
		ptp = pane_point(parent);
	if (!ptp)
		return;
	rt->top = NULL;
	rt->bot = NULL;
	rt->ignore_point = 0;
	rt->target_x = -1;
	rt->type.func = render_text_notify;
	rt->typenum = doc_add_view((*ptp)->doc, &rt->type);
	p = pane_register(parent, 0, &render_text_handle, rt, NULL);
	rt->pane = p;

	if (!rt_map)
		render_text_register_map();
}
static int do_render_text_attach(struct command *c, struct cmd_info *ci)
{
	render_text_attach(ci->focus, ci->pointp);
	return 1;
}
DEF_CMD(comm_attach, do_render_text_attach);

void edlib_init(struct editor *ed)
{
	key_add(ed->commands, "render-text-attach", &comm_attach);
}
