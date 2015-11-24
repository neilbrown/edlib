/*
 * Copyright Neil Brown <neil@brown.name> 2015
 * May be distributed under terms of GPLv2 - see file:COPYING
 *
 * render directory listing.
 *
 * One line per entry (for now).
 */

#include <unistd.h>
#include <stdlib.h>
#include <wchar.h>
#include <string.h>
#include <ctype.h>

#include "core.h"

struct dir_data {
	struct mark	*top, *bot;
	int		ignore_point;
	struct command	type;
	int		typenum;
	struct pane	*pane;
	int		header;
	short		fields;
	short		home_field;
};

static struct map *dr_map;
static struct pane *do_render_dir_attach(struct pane *parent, struct point **ptp);

static int put_str(struct pane *p, char *buf, char *attrs, int x, int y)
{
	int len = 0;
	while (buf[len]) {
		pane_text(p, buf[len], attrs, x, y);
		x += 1;
		len += 1;
	}
	return len;
}

static struct mark *render(struct point **ptp, struct pane *p)
{
	struct mark *m;
	struct mark *last_vis;
	struct dir_data *dd = p->data;
	struct doc *d = (*ptp)->doc;
	int x = 0, y = 0;
	char *hdr;
	char *body;

	pane_clear(p, NULL);

	hdr = doc_attr(d, NULL, 0, "heading");
	body = doc_attr(d, NULL, 0, "line-format");
	if (hdr) {
		put_str(p, hdr, "bold", x, y);
		y += 1;
		dd->header = 1;
	} else
		dd->header = 0;
	if (!body)
		body = "%name";

	m = mark_dup(dd->top, 0);
	last_vis = mark_dup(m, 00);

	p->cx = -1;
	p->cy = -1;

	while (y < p->h) {
		wint_t ch;
		char *name, *n;
		int home = -1;
		int field = 0;

		mark_free(last_vis);
		last_vis = mark_dup(m, 0);
		if (mark_same(d, m, &(*ptp)->m)) {
			p->cx = x;
			p->cy = y;
		}
		ch = mark_next(d, m);
		if (ch == WEOF)
			break;
		n = body;
		while (*n) {
			char buf[40], *b;
			int w, adjust, l;

			if (*n != '%' || n[1] == '%') {
				pane_text(p, *n, NULL, x, y);
				if (*n == '%')
					n += 1;
				x += 1;
				n += 1;
				continue;
			}
			field += 1;
			n += 1;
			if (*n == '+') {
				/* Home field */
				n += 1;
				home = field;
				if (dd->home_field < 0)
					dd->home_field = home;
			}
			if (p->cy == y && (*ptp)->m.rpos == field - dd->home_field)
				p->cx = x;
			b = buf;
			while (*n == '-' || *n == '_' || isalnum(*n)) {
				if (b < buf + sizeof(buf) - 2)
					*b++ = *n;
				n += 1;
			}
			*b = 0;
			if (strcmp(buf, "c") == 0) {
				/* Display the char under cursor */
				pane_text(p, ch, "fg:red", x, y);
				x += 1;
				continue;
			}
			name = doc_attr(d, m, 0, buf);
			if (!name)
				name = "-";
			if (*n != ':') {
				char *attr = NULL;
				if (home == field)
					attr = "fg:blue";
				x += put_str(p, name, attr, x, y);
				continue;
			}
			w = 0;
			adjust=0;
			n += 1;
			while (*n) {
				if (isdigit(*n))
					w = w * 10 + (*n - '0');
				else if (w == 0 && *n == '-')
					adjust = 1;
				else break;
				n+= 1;
			}
			l = strlen(name);
			while (adjust && w > l) {
				pane_text(p, ' ', NULL, x, y);
				x += 1;
				w -= 1;
			}

			while (*name && w > 0 ) {
				char *attr = NULL;
				if (home == field)
					attr = "fg:blue";
				pane_text(p, *name, attr, x, y);
				x += 1;
				w -= 1;
				name += 1;
			}
			while (w > 0) {
				pane_text(p, ' ', NULL, x, y);
				x += 1;
				w -= 1;
			}
		}
		dd->fields = field;
		dd->home_field = home;
		y += 1;
		x = 0;
	}
	mark_free(m);
	if (mark_ordered(&(*ptp)->m, dd->top) && !mark_same(d, &(*ptp)->m, dd->top))
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

static struct mark *find_top(struct point **ptp, struct pane *p,
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
	struct doc *d = (*ptp)->doc;
	int found_start = 0, found_end = 0;
	int ph = p->h - dd->header;
	int height = 0;

	start = mark_at_point(*ptp, dd->typenum);
	end = mark_at_point(*ptp, dd->typenum);
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

DEF_CMD(render_dir_handle)
{
	struct pane *p = ci->home;
	struct dir_data *dd = p->data;
	struct mark *end = NULL, *top;
	struct doc *d;
	int ret;

	ret = key_lookup(dr_map, ci);
	if (ret)
		return ret;

	if (strcmp(ci->key, "Close") == 0) {
		struct pane *p = dd->pane;
		d = (*ci->pointp)->doc;
		mark_free(dd->top);
		mark_free(dd->bot);
		dd->pane = NULL;
		doc_del_view(d, &dd->type);
		p->data = NULL;
		p->handle = NULL;
		free(dd);
		return 1;
	}
	if (strcmp(ci->key, "Clone") == 0) {
		struct pane *parent = ci->focus;
		struct pane *c;

		do_render_dir_attach(parent, NULL);
		c = pane_child(p);
		if (c)
			return pane_clone(c, parent->focus);
		return 1;
	}
	if (strcmp(ci->key, "Refresh") == 0) {

		pane_check_size(p);

		if (dd->top) {
			end = render(ci->pointp, p);
			if (dd->ignore_point || p->cx >= 0)
				goto found;
		}
		top = find_top(ci->pointp, p, dd->top, end);
		mark_free(dd->top);
		mark_free(end);
		dd->top = top;
		end = render(ci->pointp, p);
	found:
		mark_free(dd->bot);
		dd->bot = end;
		return 1;
	}
	return 0;
}

DEF_CMD(render_dir_notify)
{
	struct dir_data *dd = container_of(ci->comm, struct dir_data, type);

	if (strcmp(ci->key, "Replace") == 0) {
		if (ci->mark == dd->top)
			/* A change in the text between top and bot */
			pane_damaged(dd->pane, DAMAGED_CONTENT);
		return 0;
	}
	if (strcmp(ci->key, "Release") == 0) {
		if (dd->pane)
			pane_close(dd->pane);
		return 1;
	}
	return 0;
}

DEF_CMD(render_dir_move)
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
	pane_damaged(p, DAMAGED_CONTENT);
	return 1;
}

DEF_CMD(render_dir_follow_point)
{
	struct pane *p = ci->home;
	struct dir_data *dd = p->data;

	if (dd->ignore_point) {
		dd->ignore_point = 0;
		pane_damaged(p, DAMAGED_CONTENT);
	}
	return 0;
}

DEF_CMD(render_dir_set_cursor)
{
	struct pane *p = ci->home;
	struct point *pt = *ci->pointp;
	struct mark *m;

	m = find_pos(pt->doc, p, ci->hx, ci->hy);
	point_to_mark(pt, m);
	mark_free(m);
	pane_focus(p);
	return 1;
}

DEF_CMD(render_dir_move_line)
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

DEF_CMD(render_dir_move_horiz)
{
	/* Horizonal movement - adjust ->rpos within fields, or
	 * move to next line
	 */
	struct point *pt = *ci->pointp;
	struct dir_data *dd = ci->home->data;
	int rpt = RPT_NUM(ci);

	if (dd->fields < 2)
		return 0;
	while (rpt > 0 && doc_following(pt->doc, ci->mark) != WEOF) {
		if (ci->mark->rpos < dd->fields - dd->home_field)
			ci->mark->rpos += 1;
		else {
			if (mark_next(pt->doc, ci->mark) == WEOF)
				break;
			ci->mark->rpos = -dd->home_field;
		}
		rpt -= 1;
	}
	while (rpt < 0) {
		if (ci->mark->rpos > - dd->home_field)
			ci->mark->rpos -= 1;
		else {
			if (mark_prev(pt->doc, ci->mark) == WEOF)
				break;
			ci->mark->rpos = dd->fields - dd->home_field;
		}
		rpt += 1;
	}
	return 1;
}

DEF_CMD(render_dir_open)
{
	struct cmd_info ci2 = *ci;

	ci2.key = "Open";
	if (strcmp(ci->key, "Chr-h") == 0)
		ci2.str = "hex";
	return key_handle_focus(&ci2);
}

DEF_CMD(render_dir_reload)
{
	struct point **ptp = ci->pointp;
	struct doc *d;

	if (!ptp)
		return 0;
	d = (*ptp)->doc;
	if (d->ops->load_file)
		d->ops->load_file(d, NULL, -1, NULL);
	return 1;
}
static void render_dir_register_map(void)
{
	dr_map = key_alloc();

	key_add_range(dr_map, "Move-", "Move-\377", &render_dir_follow_point);
	key_add(dr_map, "Move-View-Small", &render_dir_move);
	key_add(dr_map, "Move-View-Large", &render_dir_move);
	key_add(dr_map, "Move-CursorXY", &render_dir_set_cursor);
	key_add(dr_map, "Click-1", &render_dir_set_cursor);
	key_add(dr_map, "Press-1", &render_dir_set_cursor);
	key_add(dr_map, "Move-Line", &render_dir_move_line);
	key_add(dr_map, "Move-Char", &render_dir_move_horiz);
	key_add(dr_map, "Move-Word", &render_dir_move_horiz);
	key_add(dr_map, "Move-WORD", &render_dir_move_horiz);

	key_add(dr_map, "Replace", &render_dir_follow_point);

	key_add(dr_map, "Chr-f", &render_dir_open);
	key_add(dr_map, "Chr-h", &render_dir_open);
	key_add(dr_map, "Chr-g", &render_dir_reload);
}

static struct pane *do_render_dir_attach(struct pane *parent, struct point **ptp)
{
	struct dir_data *dd = malloc(sizeof(*dd));
	struct pane *p;

	if (!ptp)
		ptp = pane_point(parent);
	if (!ptp)
		return NULL;
	dd->top = NULL;
	dd->bot = NULL;
	dd->ignore_point = 0;
	dd->type = render_dir_notify;
	dd->typenum = doc_add_view((*ptp)->doc, &dd->type);
	p = pane_register(parent, 0, &render_dir_handle, dd, NULL);
	dd->pane = p;
	dd->header = 0;
	dd->home_field = -1;

	if (!dr_map)
		render_dir_register_map();
	return p;
}

DEF_CMD(render_dir_attach)
{
	ci->focus = do_render_dir_attach(ci->focus, ci->pointp);
	return 1;
}

void edlib_init(struct editor *ed)
{
	key_add(ed->commands, "render-dir-attach", &render_dir_attach);
}
