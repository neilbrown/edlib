/*
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

#include "core.h"
#include "pane.h"
#include "view.h"
#include "attr.h"

#include "extras.h"

struct he_data {
	struct mark	*top, *bot;
	int		ignore_point;
	struct command	type;
	int		typenum;
	struct pane	*pane;
};

static struct map *he_map;

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
	struct he_data *he = p->data;
	struct doc *d = pt->doc;
	int x = 0, y = 0;
	struct mark *m;
	int c;

	pane_clear(p, 0, 0, 0, 0, 0);

	count_calculate(pt->doc, NULL, he->top);
	c = attr_find_int(*mark_attr(he->top), "chars");

	m = mark_dup(he->top, 0);

	p->cx = -1;
	p->cy = -1;

	for (y = 0; y < p->h; y++) {
		int xcol = 0;
		int ccol = 10+16*3+2+1;
		char buf[20];

		sprintf(buf, "%08x: ", c);
		xcol += put_str(p, buf, 0, xcol, y);
		for (x = 0; x < 16; x++) {
			wint_t ch;
			if (mark_same(d, m, mark_of_point(pt))) {
				p->cx = xcol;
				p->cy = y;
			}
			ch = mark_next(d, m);
			if (ch == WEOF)
				break;
			sprintf(buf, "%02x ", ch & 0xff);
			xcol += put_str(p, buf, 0, xcol, y);
			if (x == 7)
				xcol += 1;

			if (ch < ' ')
				ch = '?';
			pane_text(p, ch, 0, ccol, y);
			ccol += 1;
			if (x == 7)
				ccol += 1;
		}
		c += x;
		if (x < 16)
			break;
	}
	if (mark_ordered(mark_of_point(pt), he->top) &&
	    !mark_same(d, mark_of_point(pt), he->top))
		p->cx = p->cy = -1;
	return m;
}

static struct mark *find_top(struct point *pt, struct pane *p,
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
	struct doc *d = pt->doc;

	count_calculate(d, NULL, mark_of_point(pt));
	point_pos = attr_find_int(*mark_attr(mark_of_point(pt)), "chars");
	tpos = bpos = ppos = point_pos;
	if (top) {
		count_calculate(d, NULL, top);
		tpos = attr_find_int(*mark_attr(top), "chars");
	}
	if (bot) {
		count_calculate(d, NULL, bot);
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
	m = mark_at_point(pt, he->typenum);

	while (pos < point_pos) {
		mark_prev(d, m);
		point_pos -= 1;
	}
	return m;
}

static int do_render_hex_refresh(struct command *c, struct cmd_info *ci)
{
	struct pane *p = ci->focus;
	struct he_data *he = p->data;
	struct mark *end = NULL, *top;
	struct point *pt = ci->point_pane->point;

	if (strcmp(ci->key, "Close") == 0) {
		struct point *pt = ci->point_pane->point;
		struct pane *p = he->pane;
		mark_free(he->top);
		mark_free(he->bot);
		doc_del_view(pt->doc, &he->type);
		p->data = NULL;
		p->refresh = NULL;
		p->keymap = NULL;
		free(he);
		return 1;
	}
	if (strcmp(ci->key, "Refresh") != 0)
		return 0;

	if (p->focus == NULL && !list_empty(&p->children))
		p->focus = list_first_entry(&p->children, struct pane, siblings);
	if (he->top) {
		end = render(pt, p);
		if (he->ignore_point || p->cx >= 0)
			goto found;
	}
	top = find_top(pt, p, he->top, end);
	mark_free(he->top);
	mark_free(end);
	he->top = top;
	end = render(pt, p);
found:
	mark_free(he->bot);
	he->bot = end;
	return 0;
}
DEF_CMD(render_hex_refresh, do_render_hex_refresh, "render-hex-refresh");

static int render_hex_notify(struct command *c, struct cmd_info *ci)
{
	struct he_data *he = container_of(c, struct he_data, type);

	if (strcmp(ci->key, "Replace") != 0)
		return 0;
	if (ci->mark == he->top)
		/* A change in the text between top and bot */
		pane_damaged(he->pane, DAMAGED_CONTENT);
	return 0;
}

static int render_hex_move(struct command *c, struct cmd_info *ci)
{
	struct pane *p = ci->focus;
	int rpt = RPT_NUM(ci);
	struct he_data *he = p->data;
	struct point *pt = ci->point_pane->point;

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
	pane_damaged(p, DAMAGED_CURSOR);
	return 1;
}
DEF_CMD(comm_move, render_hex_move, "move-view");

static int render_hex_follow_point(struct command *c, struct cmd_info *ci)
{
	struct pane *p = ci->focus;
	struct he_data *he = p->data;

	if (he->ignore_point) {
		pane_damaged(p, DAMAGED_CURSOR);
		he->ignore_point = 0;
	}
	return 0;
}
DEF_CMD(comm_follow, render_hex_follow_point, "follow-point");

static int render_hex_set_cursor(struct command *c, struct cmd_info *ci)
{
	struct pane *p = ci->focus;
	struct point *pt = ci->point_pane->point;
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
DEF_CMD(comm_cursor, render_hex_set_cursor, "set-cursor");

static int render_hex_move_line(struct command *c, struct cmd_info *ci)
{
	/* MV_CHAR 16 times repeat count */
	struct cmd_info ci2 = {0};

	ci2 = *ci;
	ci2.key = "Move-Char";
	ci2.numeric = RPT_NUM(ci) * 16;
	return key_handle_focus(&ci2);

}
DEF_CMD(comm_line, render_hex_move_line, "move-line");

static int render_hex_eol(struct command *c, struct cmd_info *ci)
{
	struct point *pt = ci->point_pane->point;
	wint_t ch = 1;
	int rpt = RPT_NUM(ci);
	int pos;
	struct he_data *he = ci->focus->data;

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
DEF_CMD(comm_eol, render_hex_eol, "move-end-of-line");

static void render_hex_register(void)
{
	he_map = key_alloc();

	key_add_range(he_map, "Move-", "Move-\377", &comm_follow);
	key_add(he_map, "Move-View-Small", &comm_move);
	key_add(he_map, "Move-View-Large", &comm_move);
	key_add(he_map, "Move-CursorXY", &comm_cursor);
	key_add(he_map, "Click-1", &comm_cursor);
	key_add(he_map, "Press-1", &comm_cursor);
	key_add(he_map, "Move-Line", &comm_line);

	key_add(he_map, "Move-EOL", &comm_eol);
	key_add(he_map, "Replace", &comm_follow);
}

void render_hex_attach(struct pane *p)
{
	struct he_data *he = malloc(sizeof(*he));

	he->pane = p;
	he->top = NULL;
	he->bot = NULL;
	he->ignore_point = 0;
	he->type.func = render_hex_notify;
	he->type.name = "render_hex_notify";
	he->typenum = doc_add_view(p->parent->point->doc, &he->type);
	p->data = he;
	p->refresh = &render_hex_refresh;
	if (!he_map)
		render_hex_register();
	p->keymap = he_map;
}
