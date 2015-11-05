/*
 * render directory listing.
 *
 * One line per entry (for now).
 */

#include <unistd.h>
#include <stdlib.h>
#include <wchar.h>
#include <string.h>

#include "core.h"
#include "pane.h"
#include "view.h"
#include "attr.h"

#include "extras.h"

struct dir_data {
	struct mark	*top, *bot;
	int		ignore_point;
	struct command	type;
	int		typenum;
	struct pane	*pane;
	int		header;
};

static struct map *dr_map;
static void render_dir_attach(struct pane *parent, struct point *pt);

static int put_str(struct pane *p, char *buf, int attr, int x, int y)
{
	int len = 0;
	while (buf[len]) {
		pane_text(p, buf[len], attr, x, y);
		x += 1;
		len += 1;
	}
	return len;
}

static struct mark *render(struct point *pt, struct pane *p)
{
	struct mark *m;
	struct mark *last_vis;
	struct dir_data *dd = p->data;
	struct doc *d = pt->doc;
	int x = 0, y = 0;
	char *hdr;

	pane_clear(p, 0, 0, 0, 0, 0);

	hdr = doc_attr(d, NULL, 0, "heading");
	if (hdr) {
		put_str(p, hdr, 1<<(8+13), x, y);
		y += 1;
		dd->header = 1;
	} else
		dd->header = 0;

	m = mark_dup(dd->top, 0);
	last_vis = mark_dup(m, 00);

	p->cx = -1;
	p->cy = -1;

	while (y < p->h) {
		wint_t ch;
		char *name;

		mark_free(last_vis);
		last_vis = mark_dup(m, 0);
		if (mark_same(d, m, mark_of_point(pt))) {
			p->cx = x;
			p->cy = y;
		}
		ch = mark_next(d, m);
		if (ch == WEOF)
			break;
		name = doc_attr(d, m, 0, "name");
		put_str(p, name, 0, x, y);
		y += 1;
	}
	mark_free(m);
	if (mark_ordered(mark_of_point(pt), dd->top) && !mark_same(d, mark_of_point(pt), dd->top))
		p->cx = p->cy = -1;
	return last_vis;
}

static struct mark *find_pos(struct doc *d, struct pane *p, int px, int py)
{
	struct mark *m;
	struct dir_data *dd = p->data;

	if (dd->header)
		py -= 1;
	m = mark_dup(dd->top, 1);
	while (py > 0) {
		mark_next(d, m);
		py -= 1;
	}
	return m;
}

static struct mark *find_top(struct point *pt, struct pane *p,
			     struct mark *top, struct mark *bot)
{
	/* If top and bot are not NULL, they record what is currently
	 * visible.  We walk out from point until we reach extremes of
	 * buffer or cross top (from above) or bot (from below).
	 * When end hits EOF or start crosses bot, end stops moving.
	 * When number of entries reaches height of pane, both stop moving.
	 * At tis point, 'start' is the new 'top'.
	 */
	struct dir_data *dd = p->data;
	struct mark *start, *end;
	struct doc *d = pt->doc;
	int found_start = 0, found_end = 0;
	int ph = p->h - dd->header;
	int height = 0;

	start = mark_at_point(pt, dd->typenum);
	end = mark_at_point(pt, dd->typenum);
	if (bot &&
	    (mark_ordered(start, bot) && ! mark_same(d, start, bot)))
		/* We can never cross bot from below */
		bot = NULL;
	if (top &&
	    (mark_ordered(top, end) && ! mark_same(d, top, end)))
		/* We can never cross top from above */
		top = NULL;
	while (!((found_start && found_end) || height >= ph-1)) {
		if (!found_start) {
			if (doc_prior(d, start) == WEOF)
				found_start = 1;
			else {
				mark_prev(d, start);
				height += 1;
			}

			if (bot && mark_ordered(start, bot))
				found_end = 1;
		}
		if (!found_end) {
			if (mark_next(d, end) == WEOF)
				found_end = 1;
			else
				height += 1;

			if (top && mark_ordered(top, end))
				found_start = 1;
		}
	}
	mark_free(end);
	return start;
}

static int do_render_dir_refresh(struct command *c, struct cmd_info *ci)
{
	struct pane *p = ci->home;
	struct dir_data *dd = p->data;
	struct mark *end = NULL, *top;
	struct point *pt;

	if (strcmp(ci->key, "Close") == 0) {
		struct pane *p = dd->pane;
		pt = *ci->pointp;
		mark_free(dd->top);
		mark_free(dd->bot);
		doc_del_view(pt->doc, &dd->type);
		p->data = NULL;
		p->refresh = NULL;
		p->keymap = NULL;
		free(dd);
		return 1;
	}
	if (strcmp(ci->key, "Clone") == 0) {
		struct pane *parent = ci->focus;
		struct pane *pp = parent;
		while (pp && pp->point == NULL)
			pp = pp->parent;
		if (!pp)
			return 0;
		render_dir_attach(parent, pp->point);
		if (p->focus)
			return pane_clone(p->focus, parent->focus);
		return 1;
	}
	if (strcmp(ci->key, "Refresh") != 0)
		return 0;

	pt = *ci->pointp;
	pane_check_size(p);

	if (p->focus == NULL && !list_empty(&p->children))
		p->focus = list_first_entry(&p->children, struct pane, siblings);
	if (dd->top) {
		end = render(pt, p);
		if (dd->ignore_point || p->cx >= 0)
			goto found;
	}
	top = find_top(pt, p, dd->top, end);
	mark_free(dd->top);
	mark_free(end);
	dd->top = top;
	end = render(pt, p);
found:
	mark_free(dd->bot);
	dd->bot = end;
	return 0;
}
DEF_CMD(render_dir_refresh, do_render_dir_refresh, "rendier-dir-refresh");

static int render_dir_notify(struct command *c, struct cmd_info *ci)
{
	struct dir_data *dd = container_of(c, struct dir_data, type);

	if (strcmp(ci->key, "Replace") != 0)
		return 0;
	if (ci->mark == dd->top)
		/* A change in the text between top and bot */
		pane_damaged(dd->pane, DAMAGED_CONTENT);
	return 0;
}

static int render_dir_move(struct command *c, struct cmd_info *ci)
{
	struct pane *p = ci->home;
	int rpt = RPT_NUM(ci);
	struct dir_data *dd = p->data;
	struct point *pt = *ci->pointp;

	if (!dd->top)
		return 0;
	if (strcmp(ci->key, "Move-View-Large") == 0)
		rpt *= p->h - 2;
	dd->ignore_point = 1;
	while (rpt > 0) {
		if (mark_next(pt->doc, dd->top) == WEOF)
			break;
		rpt -= 1;
	}
	while (rpt < 0) {
		if (mark_prev(pt->doc, dd->top) == WEOF)
			break;
		rpt += 1;
	}
	pane_damaged(p, DAMAGED_CURSOR);
	return 1;
}
DEF_CMD(comm_move, render_dir_move, "move-view");

static int render_dir_follow_point(struct command *c, struct cmd_info *ci)
{
	struct pane *p = ci->home;
	struct dir_data *dd = p->data;

	if (dd->ignore_point) {
		dd->ignore_point = 0;
		pane_damaged(p, DAMAGED_CURSOR);
	}
	return 0;
}
DEF_CMD(comm_follow, render_dir_follow_point, "follow-point");

static int render_dir_set_cursor(struct command *c, struct cmd_info *ci)
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
DEF_CMD(comm_cursor, render_dir_set_cursor, "set-cursor");

static int render_dir_move_line(struct command *c, struct cmd_info *ci)
{
	struct point *pt = *ci->pointp;
	struct dir_data *dd = ci->home->data;
	int rpt = RPT_NUM(ci);

	while (rpt > 0) {
		if (mark_next(pt->doc, ci->mark) == WEOF)
			break;
		rpt -= 1;
	}
	while (rpt < 0) {
		if (mark_prev(pt->doc, ci->mark) == WEOF)
			break;
		rpt += 1;
	}
	dd->ignore_point = 0;

	return 1;
}
DEF_CMD(comm_line, render_dir_move_line, "move-line");

static void render_dir_register_map(void)
{
	dr_map = key_alloc();

	key_add_range(dr_map, "Move-", "Move-\377", &comm_follow);
	key_add(dr_map, "Move-View-Small", &comm_move);
	key_add(dr_map, "Move-View-Large", &comm_move);
	key_add(dr_map, "Move-CursorXY", &comm_cursor);
	key_add(dr_map, "Click-1", &comm_cursor);
	key_add(dr_map, "Press-1", &comm_cursor);
	key_add(dr_map, "Move-Line", &comm_line);

	key_add(dr_map, "Replace", &comm_follow);
}

static void render_dir_attach(struct pane *parent, struct point *pt)
{
	struct dir_data *dd = malloc(sizeof(*dd));
	struct pane *p;

	dd->top = NULL;
	dd->bot = NULL;
	dd->ignore_point = 0;
	dd->type.func = render_dir_notify;
	dd->type.name = "render_dir_notify";
	dd->typenum = doc_add_view(pt->doc, &dd->type);
	p = pane_register(parent, 0, &render_dir_refresh, dd, NULL);
	dd->pane = p;

	if (!dr_map)
		render_dir_register_map();
	p->keymap = dr_map;
}

struct rendertype render_dir = {
	.name	= "dir",
	.attach	= render_dir_attach,
};

void render_dir_register(struct editor *ed)
{
	render_register_type(ed, &render_dir);
}
