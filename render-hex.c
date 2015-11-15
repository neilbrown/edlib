/*
 * Copyright Neil Brown <neil@brown.name> 2015
 * May be distributed under terms of GPLv2 - see file:COPYING
 *
 * hexedit renderer
 *
 * 16 bytes are rendered as hex, and then chars
 * Well... currently we do chars, not bytes, because I cannot control
 * char encoding yet.
 *
 */

#include <unistd.h>
#include <stdlib.h>
#include <wchar.h>
#include <string.h>
#include <stdio.h>

#include "core.h"

struct he_data {
	struct mark	*top, *bot;
	int		ignore_point;
	struct command	type;
	int		typenum;
	struct pane	*pane;
};

static struct map *he_map;
static void do_render_hex_attach(struct pane *parent, struct point **ptp);

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
	struct he_data *he = p->data;
	struct doc *d = (*ptp)->doc;
	int x = 0, y = 0;
	struct mark *m;
	int c;
	struct cmd_info ci2 = {0};

	pane_clear(p, NULL);

	ci2.key = "CountLines";
	ci2.pointp = ptp;
	ci2.mark = he->top;
	key_lookup(d->ed->commands, &ci2);

	c = attr_find_int(*mark_attr(he->top), "chars");

	m = mark_dup(he->top, 0);

	p->cx = -1;
	p->cy = -1;

	for (y = 0; y < p->h; y++) {
		int xcol = 0;
		int ccol = 10+16*3+2+1;
		char buf[20];

		sprintf(buf, "%08x: ", c);
		xcol += put_str(p, buf, NULL, xcol, y);
		for (x = 0; x < 16; x++) {
			wint_t ch;
			if (mark_same(d, m, mark_of_point(*ptp))) {
				p->cx = xcol;
				p->cy = y;
			}
			ch = mark_next(d, m);
			if (ch == WEOF)
				break;
			sprintf(buf, "%02x ", ch & 0xff);
			xcol += put_str(p, buf, NULL, xcol, y);
			if (x == 7)
				xcol += 1;

			if (ch < ' ')
				ch = '?';
			pane_text(p, ch, NULL, ccol, y);
			ccol += 1;
			if (x == 7)
				ccol += 1;
		}
		c += x;
		if (x < 16)
			break;
	}
	if (mark_ordered(mark_of_point(*ptp), he->top) &&
	    !mark_same(d, mark_of_point(*ptp), he->top))
		p->cx = p->cy = -1;
	return m;
}

static struct mark *find_top(struct point **ptp, struct pane *p,
			     struct mark *top, struct mark *bot)
{
	/* top and bot might be NULL, else they record what is currently
	 * visible.
	 * We find the location of point, top, bot and then choose a new
	 * top.
	 * top must be a multiple of 16, must keep point on the pane,
	 * and should leave old values as unchanged as possible.
	 */
	struct mark *m;
	int ppos, tpos, bpos, pos, point_pos;
	struct he_data *he = p->data;
	struct doc *d = (*ptp)->doc;
	struct cmd_info ci2 = {0};

	ci2.key = "CountLines";
	ci2.pointp = ptp;
	ci2.mark = top;
	key_lookup(d->ed->commands, &ci2);
	point_pos = attr_find_int(*mark_attr(mark_of_point(*ptp)), "chars");
	tpos = bpos = ppos = point_pos;
	if (top) {
		tpos = attr_find_int(*mark_attr(top), "chars");
	}
	if (bot) {
		ci2.mark = bot;
		key_lookup(d->ed->commands, &ci2);
		bpos = attr_find_int(*mark_attr(bot), "chars");
	}
	ppos -= ppos % 16;
	tpos -= tpos % 16;
	bpos -= bpos % 16;
	if (tpos <= ppos && tpos + p->h * 16 > ppos) {
		/* point is within displayed region - no change */
		pos = tpos;
	} else if (ppos < tpos && tpos - ppos < (p->h/2) * 16) {
		/* point is less than half a pane before current display,
		 * just scroll twice the gap */
		pos = ppos - (tpos - ppos);
		if (pos < 0)
			pos = 0;
	} else if (ppos > tpos + p->h*16 && ppos - (tpos + p->h*16) < (p->h/2) * 16) {
		/* point is less than half a pane below display, so scroll
		 * twice the gap */
		pos = ppos + (ppos - (tpos + p->h*16)) - p->h*16;
	} else {
		/* to far - just re-center */
		if (ppos  < p->h/2 * 16)
			pos = 0;
		else
			pos = ppos - p->h/2 * 16;
	}
	m = mark_at_point(*ptp, he->typenum);

	while (pos < point_pos) {
		mark_prev(d, m);
		point_pos -= 1;
	}
	return m;
}

static int hex_refresh(struct cmd_info *ci)
{
	struct pane *p = ci->home;
	struct he_data *he = p->data;
	struct mark *end = NULL, *top;
	struct doc *d;

	if (!ci->pointp)
		return 0;
	d = (*ci->pointp)->doc;
	pane_check_size(p);

	if (he->top) {
		struct cmd_info ci2 = {0};
		int tpos;
		ci2.key = "CountLines";
		ci2.pointp = ci->pointp;
		ci2.mark = he->top;
		key_lookup(d->ed->commands, &ci2);
		tpos = attr_find_int(*mark_attr(he->top), "chars");
		if (tpos % 16 != 0) {
			top = find_top(ci->pointp, p, he->top, end);
			mark_free(he->top);
			he->top = top;
		}
	}

	if (he->top) {
		end = render(ci->pointp, p);
		if (he->ignore_point || p->cx >= 0)
			goto found;
	}
	top = find_top(ci->pointp, p, he->top, end);
	mark_free(he->top);
	mark_free(end);
	he->top = top;
	end = render(ci->pointp, p);
found:
	mark_free(he->bot);
	he->bot = end;
	return 0;
}

DEF_CMD(render_hex_handle)
{
	struct pane *p = ci->home;
	struct he_data *he = p->data;
	struct doc *d;
	int ret;

	ret = key_lookup(he_map, ci);
	if (ret)
		return ret;

	if (strcmp(ci->key, "Close") == 0) {
		struct pane *p = he->pane;

		d = (*ci->pointp)->doc;
		mark_free(he->top);
		mark_free(he->bot);
		he->pane = NULL;
		doc_del_view(d, &he->type);
		p->data = NULL;
		p->handle = NULL;
		free(he);
		return 1;
	}
	if (strcmp(ci->key, "Clone") == 0) {
		struct pane *parent = ci->focus;
		struct pane *c;

		do_render_hex_attach(parent, NULL);
		c = pane_child(p);
		if (c)
			return pane_clone(c, parent->focus);
		return 1;
	}
	if (strcmp(ci->key, "Refresh") == 0)
		return hex_refresh(ci);
	return 0;
}

DEF_CMD(render_hex_notify)
{
	struct he_data *he = container_of(ci->comm, struct he_data, type);

	if (strcmp(ci->key, "Replace") == 0) {
		if (he->bot == NULL || ci->mark == NULL ||
		    mark_ordered(ci->mark, he->bot))
			/* A change that was not after the bot, so offsets
			 * probably changed, so redraw
			 */
			pane_damaged(he->pane, DAMAGED_CONTENT);
		return 0;
	}
	if (strcmp(ci->key, "Release") == 0) {
		if (he->pane)
			pane_close(he->pane);
		return 1;
	}
	return 0;
}

DEF_CMD(render_hex_move)
{
	struct pane *p = ci->home;
	int rpt = RPT_NUM(ci);
	struct he_data *he = p->data;
	struct point *pt = *ci->pointp;

	if (!he->top)
		return 0;
	if (strcmp(ci->key, "Move-View-Large") == 0)
		rpt *= p->h - 2;
	rpt *= 16;
	he->ignore_point = 1;

	while (rpt < 0 && mark_prev(pt->doc, he->top) != WEOF)
		rpt += 1;
	while (rpt > 0 && mark_next(pt->doc, he->top) != WEOF)
		rpt -= 1;
	pane_damaged(p, DAMAGED_CONTENT);
	return 1;
}

DEF_CMD(render_hex_follow_point)
{
	struct pane *p = ci->home;
	struct he_data *he = p->data;

	if (he->ignore_point) {
		pane_damaged(p, DAMAGED_CONTENT);
		he->ignore_point = 0;
	}
	return 0;
}

DEF_CMD(render_hex_set_cursor)
{
	struct pane *p = ci->home;
	struct point *pt = *ci->pointp;
	struct he_data *he = p->data;
	struct mark *m;
	int n, x;

	if (!he->top)
		return 0;

	if (ci->x < 10)
		x = 0;
	else if (ci->x < 10 + 8*3)
		x = (ci->x - 10) / 3;
	else if (ci->x < 10 + 1 + 16*3)
		x = (ci->x - 11) / 3;
	else if (ci->x < 10 + 1 + 2 + 16*3 + 8)
		x = ci->x - (10+1+2+16*3);
	else if (ci->x < 10 + 1 + 2 + 16*3 + 8 + 1 + 8)
		x = ci->x - (10+1+2+16*3 + 1);
	else
		x = 15;
	n = ci->y * 16 + x;
	m = mark_dup(he->top, 1);
	while (n > 0 && mark_next(pt->doc, m) != WEOF)
		n -= 1;
	point_to_mark(pt, m);
	mark_free(m);
	pane_focus(p);
	return 1;
}

DEF_CMD(render_hex_move_line)
{
	/* MV_CHAR 16 times repeat count */
	struct cmd_info ci2 = {0};

	ci2 = *ci;
	ci2.key = "Move-Char";
	ci2.numeric = RPT_NUM(ci) * 16;
	return key_handle_focus(&ci2);

}

DEF_CMD(render_hex_eol)
{
	struct point *pt = *ci->pointp;
	wint_t ch = 1;
	int rpt = RPT_NUM(ci);
	int pos;
	struct he_data *he = ci->home->data;

	pos = attr_find_int(*mark_attr(ci->mark), "chars");
	while (rpt > 0 && ch != WEOF) {
		while ((pos & 15) != 15 &&
		       (ch = mark_next(pt->doc, ci->mark)) != WEOF)
			pos += 1;
		rpt -= 1;
		if (rpt) {
			ch = mark_next(pt->doc, ci->mark);
			pos += 1;
		}
	}
	while (rpt < 0 && ch != WEOF) {
		while ((pos & 15) != 0 &&
		       (ch = mark_prev(pt->doc, ci->mark)) != WEOF)
			pos -= 1;
		rpt += 1;
		if (rpt) {
			ch = mark_prev(pt->doc, ci->mark);
			pos -= 1;
		}
	}
	he->ignore_point = 0;
	return 1;
}

static void render_hex_register_map(void)
{
	he_map = key_alloc();

	key_add_range(he_map, "Move-", "Move-\377", &render_hex_follow_point);
	key_add(he_map, "Move-View-Small", &render_hex_move);
	key_add(he_map, "Move-View-Large", &render_hex_move);
	key_add(he_map, "Move-CursorXY", &render_hex_set_cursor);
	key_add(he_map, "Click-1", &render_hex_set_cursor);
	key_add(he_map, "Press-1", &render_hex_set_cursor);
	key_add(he_map, "Move-Line", &render_hex_move_line);

	key_add(he_map, "Move-EOL", &render_hex_eol);
	key_add(he_map, "Replace", &render_hex_follow_point);
}

static void do_render_hex_attach(struct pane *parent, struct point **ptp)
{
	struct he_data *he = malloc(sizeof(*he));
	struct pane *p;

	if (!ptp)
		ptp = pane_point(parent);
	if (!ptp)
		return;

	he->top = NULL;
	he->bot = NULL;
	he->ignore_point = 0;
	he->type = render_hex_notify;
	he->typenum = doc_add_view((*ptp)->doc, &he->type);
	p = pane_register(parent, 0, &render_hex_handle, he, NULL);
	he->pane = p;

	if (!he_map)
		render_hex_register_map();
}

DEF_CMD(render_hex_attach)
{
	do_render_hex_attach(ci->focus, ci->pointp);
	return 1;
}

void edlib_init(struct editor *ed)
{
	key_add(ed->commands, "render-hex-attach", &render_hex_attach);
}
