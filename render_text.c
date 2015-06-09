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

struct rt_data {
	struct view_data *v;
	struct mark	*top;
};

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
		ch = mark_next(t, m);
		if (ch == WEOF)
			break;
		if (ch == '\n') {
			x = 0;
			y += 1;
		} else if (ch == '\t') {
			x += 8;
			x -= x%8;
		} else {
			if (x >= p->w-1) {
				pane_text(p, '\\', A_UNDERLINE, x, y);
				y += 1;
				x = 0;
			}
			if (ch < ' ') {
				pane_text(p, '^', A_UNDERLINE, x, y);
				pane_text(p, ch+'@', A_UNDERLINE, x+1, y);
				x += 2;
			} else {
				pane_text(p, ch, 0, x, y);
				x += 1;
			}
		}
	}
	return last_vis;
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
			ch = mark_prev(t, start);
			if (ch == WEOF)
				found_start = 1;
			else if (ch == '\n') {
				sx = 0;
				sy -= 1;
			} else if (ch == '\t') {
				/* tricky... */
				sx += 6;
			} else if (ch < ' ') {
				sx += 2;
			} else {
				sx += 1;
			}
			if (sx > p->w) {
				sx = 0;
				sy -= 1;
			}
			if (bot && mark_ordered(start, bot))
				found_end = 1;
		}
		if (!found_end) {
			ch = mark_next(t, end);
			if (ch == WEOF)
				found_end = 1;
			else if (ch == '\n') {
				ex = 0;
				ey += 1;
			} else if (ch == '\t') {
				ex += 8;
				ex -= ex*8;
			} else if (ch < ' ')
				ex += 2;
			else
				ex += 1;
			if (ex > p->w) {
				ex = 0;
				ey += 1;
			}
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
		if (p->cx >= 0) {
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
	p->data = rt;
	p->refresh = render_text_refresh;
}
