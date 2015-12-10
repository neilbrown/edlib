/*
 * Copyright Neil Brown ©2015 <neil@brown.name>
 * May be distributed under terms of GPLv2 - see file:COPYING
 *
 * A buffer can be viewed in a pane.
 * The pane is (typically) a tile in a display.
 * As well as content from the buffer, a 'view' provides
 * a scroll bar and a status line.
 * These server to visually separate different views from each other.
 *
 * For now, a cheap hack to just show the scroll bar and status line.
 */

#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>
#include <stdio.h>

#include "core.h"

struct view_data {
	int		border;
	struct command	ch_notify;
	int		ch_notify_num;
	struct pane	*pane;
	int		scroll_bar_y;
};
/* 0 to 4 borders are possible */
enum {
	BORDER_LEFT	= 1,
	BORDER_RIGHT	= 2,
	BORDER_TOP	= 4,
	BORDER_BOT	= 8,
};

static struct map *view_map;
static struct pane *do_view_attach(struct pane *par, int border);

static int view_refresh(struct cmd_info *ci)
{
	struct pane *p = ci->home;
	struct view_data *vd = p->data;
	struct mark *m = ci->mark;
	int ln, l, w, c = -1;
	struct cmd_info ci2 = {0};
	char msg[100];
	int i;
	int mid;
	struct doc *d = doc_from_pane(ci->home);

	pane_check_size(p);
	p->cx = 0; p->cy = 0;
	if (!vd->border)
		return 1;

	if (vd->border & BORDER_LEFT) {
		/* Left border is (currently) always a scroll bar */
		for (i = 0; i < p->h; i++)
			pane_text(p, '|', "inverse", 0, i);

		if (p->h > 4) {
			ci2.key = "CountLines";
			ci2.home = ci2.focus = p;
			ci2.mark = m;
			key_handle(&ci2);

			ln = attr_find_int(*mark_attr(m), "lines");
			l = pane_attr_get_int(ci->home, "lines");
			w = pane_attr_get_int(ci->home, "words");
			c = pane_attr_get_int(ci->home, "chars");
			if (l <= 0)
				l = 1;
			mid = 1 + (p->h-4) * ln / l;
			pane_text(p, '^', 0, 0, mid-1);
			pane_text(p, '#', "inverse", 0, mid);
			pane_text(p, 'v', 0, 0, mid+1);
			pane_text(p, '+', "inverse", 0, p->h-1);
			vd->scroll_bar_y = mid;
		}
	}
	if (vd->border & BORDER_RIGHT) {
		for (i = 0; i < p->h; i++)
			pane_text(p, '|', "inverse", p->w-1, i);
	}
	if (vd->border & BORDER_TOP) {
		int label;
		for (i = 0; i < p->w; i++)
			pane_text(p, '-', "inverse", i, 0);
		snprintf(msg, sizeof(msg), "%s", d->name);
		label = (p->w - strlen(msg)) / 2;
		if (label < 1)
			label = 1;
		for (i = 0; msg[i]; i++)
			pane_text(p, msg[i], "inverse", label+i, 0);
	}
	if (vd->border & BORDER_BOT) {
		for (i = 0; i < p->w; i++)
			pane_text(p, '=', "inverse", i, p->h-1);

		if (!(vd->border & BORDER_TOP)) {
			if (c >= 0)
				snprintf(msg, sizeof(msg), "L%d W%d C%d D:%s",
					 l,w,c, d->name);
			else
				snprintf(msg, sizeof(msg),"%s", d->name);
			for (i = 0; msg[i] && i+4 < p->w; i++)
				pane_text(p, msg[i], "inverse", i+4, p->h-1);
		}
	}
	if (!(~vd->border & (BORDER_LEFT|BORDER_BOT)))
		/* Both are set */
		pane_text(p, '+', "inverse", 0, p->h-1);
	if (!(~vd->border & (BORDER_RIGHT|BORDER_TOP)))
		pane_text(p, 'X', "inverse", p->w-1, 0);
	if (!(~vd->border & (BORDER_LEFT|BORDER_TOP)))
		pane_text(p, '/', "inverse", 0, 0);
	if (!(~vd->border & (BORDER_RIGHT|BORDER_BOT)))
		pane_text(p, '/', "inverse", p->w-1, p->h-1);
	return 1;
}

DEF_CMD(view_handle)
{
	struct pane *p = ci->home;
	struct view_data *vd = p->data;
	int ret;

	ret = key_lookup(view_map, ci);
	if (ret)
		return ret;

	if (vd->pane != p)
		vd->pane = p; /* FIXME having to do this is horrible */

	if (strcmp(ci->key, "Close") == 0) {
		doc_del_view(p, &vd->ch_notify);
		free(vd);
		return 1;
	}
	if (strcmp(ci->key, "Clone") == 0) {
		struct pane *parent = ci->focus;
		struct pane *p2, *c;

		p2 = do_view_attach(parent, vd->border);
		c = pane_child(pane_child(p));
		if (c)
			return pane_clone(c, pane_final_child(p2));
		return 1;
	}
	if (strcmp(ci->key, "Refresh") == 0)
		return view_refresh(ci);
	return 0;
}

DEF_CMD(view_null)
{
	struct pane *p = ci->home;
	struct view_data *vd = p->data;

	if (strcmp(ci->key, "Refresh") == 0) {
		int damage = ci->extra;
		if (damage & DAMAGED_SIZE) {
			int x = 0, y = 0;
			int w = p->parent->w;
			int h = p->parent->h;

			if (vd->border & BORDER_LEFT) {
				x += 1; w -= 1;
			}
			if (vd->border & BORDER_RIGHT) {
				w -= 1;
			}
			if (vd->border & BORDER_TOP) {
				y += 1; h -= 1;
			}
			if (vd->border & BORDER_BOT) {
				h -= 1;
			}
			pane_resize(p, x, y, w, h);
		}
		return 1;
	}
	return 0;
}

static struct pane *view_reattach(struct pane *p);

DEF_CMD(view_notify)
{
	struct view_data *vd = container_of(ci->comm, struct view_data, ch_notify);

	if (strcmp(ci->key, "Notify:Replace") == 0) {
		pane_damaged(vd->pane, DAMAGED_CONTENT);
		return 0;
	}
	if (strcmp(ci->key, "Release") == 0) {
		pane_close(vd->pane);
		return 1;
	}
	return 0;
}

static struct pane *view_reattach(struct pane *par)
{
	struct view_data *vd = par->data;
	struct pane *p;

	vd->ch_notify_num = doc_add_view(par, &vd->ch_notify, 0);

	p = pane_register(par, 0, &view_null, vd, NULL);
	pane_damaged(p, DAMAGED_SIZE);
	return p;
}

static struct pane *do_view_attach(struct pane *par, int border)
{
	struct view_data *vd;
	struct pane *p;

	vd = malloc(sizeof(*vd));
	vd->border = border;
	vd->ch_notify = view_notify;
	p = pane_register(par, 0, &view_handle, vd, NULL);
	vd->pane = p;
	pane_check_size(p);

	view_reattach(p);
	return p;
}

DEF_CMD(view_attach)
{
	int borders = 0;
	char *borderstr = pane_attr_get(ci->focus, "borders");
	if (!borderstr)
		borderstr = "";
	if (strchr(borderstr, 'T')) borders |= BORDER_TOP;
	if (strchr(borderstr, 'B')) borders |= BORDER_BOT;
	if (strchr(borderstr, 'L')) borders |= BORDER_LEFT;
	if (strchr(borderstr, 'R')) borders |= BORDER_RIGHT;

	return comm_call(ci->comm2, "callback:attach", do_view_attach(ci->focus, borders),
			 0, NULL, NULL, 0);
}

DEF_CMD(view_click)
{
	struct pane *p = ci->home;
	struct view_data *vd = p->data;
	int mid = vd->scroll_bar_y;
	char *key;
	int num;

	if (ci->hx != 0)
		return 0;
	if (p->h <= 4)
		return 0;

	p = pane_child(p);

	key = "Move-View-Small";
	num = RPT_NUM(ci);

	if (ci->hy == mid-1) {
		/* scroll up */
		num = -num;
	} else if (ci->hy < mid-1) {
		/* big scroll up */
		num = -num;
		key = "Move-View-Large";
	} else if (ci->hy == mid+1) {
		/* scroll down */
	} else if (ci->hy > mid+1 && ci->hy < p->h-1) {
		key = "Move-View-Large";
	} else
		return 0;
	return call3(key, p, num, NULL);
}

void edlib_init(struct editor *ed)
{
	view_map = key_alloc();

	key_add(view_map, "Click-1", &view_click);
	key_add(view_map, "Press-1", &view_click);

	key_add(ed->commands, "attach-view", &view_attach);
}
