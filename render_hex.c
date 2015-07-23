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
#include <curses.h>

#include "core.h"
#include "pane.h"
#include "view.h"
#include "keymap.h"

#include "extras.h"

struct he_data {
	struct view_data *v;
	struct mark	*top, *bot;
	int		ignore_point;
	struct command	type;
	int		typenum;
	struct pane	*pane;
};

static struct map *he_map;

static int render_hex_refresh(struct pane *p, int damage);
#define	CMD(func, name) {func, name, render_hex_refresh}
#define	DEF_CMD(comm, func, name) static struct command comm = CMD(func, name)

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

static struct mark *render(struct doc *d, struct point *pt, struct pane *p)
{
	struct he_data *he = p->data;
	int x = 0, y = 0;
	struct mark *m;
	int l,w,c;

	pane_clear(p, 0, 0, 0, 0, 0);

	count_calculate(he->v->doc, NULL, he->top, &l , &w, &c);

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
			wint_t ch = mark_next(d, m);
			if (ch == WEOF)
				break;
			if (mark_same(d, m, mark_of_point(pt))) {
				p->cx = xcol;
				p->cy = y;
			}
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
	if (mark_ordered(mark_of_point(pt), he->top))
		p->cx = p->cy = -1;
	return m;
}

static struct mark *find_top(struct doc *d, struct point *pt, struct pane *p,
			     struct mark *top, struct mark *bot)
{
	/* top and bot might be NULL, else they record what is currently
	 * visible.
	 * We find the location of point, top, bot and then choose a new
	 * top.
	 * top must be a multiple of 16, must keep point on the pane,
	 * and should leave old values as unchanged as possible.
	 */
	int l,w;
	struct mark *m;
	int ppos, tpos, bpos, pos, tpos2;
	struct he_data *he = p->data;

	count_calculate(d, NULL, mark_of_point(pt), &l, &w, &ppos);
	if (top)
		count_calculate(d, NULL, top, &l, &w, &tpos);
	else
		tpos = ppos;
	if (bot)
		count_calculate(d, NULL, bot, &l, &w, &bpos);
	else
		bpos = ppos;
	tpos2 = tpos;
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
	} else if (ppos > tpos + p->h*!6 && ppos - (tpos + p->h*16) > (p->h/2) * 16) {
		/* point is less than half a pane below display */
		pos = ppos + (ppos - tpos + p->h*16) - p->h*16;
	} else {
		/* to far - just re-center */
		if (ppos  < p->h/2 * 16)
			pos = 0;
		else
			pos = ppos - p->h/2 * 16;
	}
	m = mark_at_point(pt, he->typenum);

	while (pos < tpos2) {
		mark_prev(d, m);
		tpos2 -= 1;
	}
	return m;
}

static int render_hex_refresh(struct pane *p, int damage)
{
	struct he_data *he = p->data;
	struct mark *end = NULL, *top;

	if (he->top) {
		end = render(he->v->doc, he->v->point, p);
		if (he->ignore_point || p->cx >= 0)
			goto found;
	}
	top = find_top(he->v->doc, he->v->point, p, he->top, end);
	mark_free(he->top);
	mark_free(end);
	he->top = top;
	end = render(he->v->doc, he->v->point, p);
found:
	mark_free(he->bot);
	he->bot = end;
	return 0;
}

static int render_hex_notify(struct command *c, struct cmd_info *ci)
{
	struct he_data *he;
	if (ci->key != EV_REPLACE)
		return 0;
	he = container_of(c, struct he_data, type);
	if (ci->mark == he->top)
		/* A change in the text between top and bot */
		pane_damaged(he->pane, DAMAGED_CONTENT);
	return 0;
}

void render_hex_attach(struct pane *p)
{
	struct he_data *he = malloc(sizeof(*he));

	he->v = p->data;
	he->pane = p;
	he->top = NULL;
	he->bot = NULL;
	he->ignore_point = 0;
	he->type.func = render_hex_notify;
	he->type.name = "render_hex_notify";
	he->type.type = NULL;
	he->typenum = doc_add_type(he->v->doc, &he->type);
	p->data = he;
	p->refresh = render_hex_refresh;
	p->keymap = he_map;
}

void render_hex_register(struct map *m)
{
	he_map = key_alloc();
}
