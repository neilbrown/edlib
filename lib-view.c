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
	int		old_border;
	int		border_width, border_height;
	int		line_height;
	int		ascent;
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

static void one_char(struct pane *p, char *s, char *attr, int x, int y)
{
	call_xy("text-display", p, -1, s, attr, x, y);
}

DEF_CMD(text_size_callback)
{
	struct call_return *cr = container_of(ci->comm, struct call_return, c);
	cr->x = ci->x;
	cr->y = ci->y;
	cr->i = ci->extra;
	return 1;
}

static int view_refresh(const struct cmd_info *ci)
{
	struct pane *p = ci->home;
	struct view_data *vd = p->data;
	struct mark *m = ci->mark;
	int ln, l, w, c = -1;
	struct cmd_info ci2 = {0};
	char msg[100];
	int i;
	int mid;
	char *name;

	pane_check_size(p);
	p->cx = 0; p->cy = 0;
	if (!vd->border)
		return 1;

	if (vd->line_height < 0) {
		struct call_return cr;
		cr.c = text_size_callback;
		call_comm7("text-size", ci->home, -1, NULL,
			   "M", 0, "bold", &cr.c);
		vd->line_height = cr.y;
		vd->border_height = cr.y;
		vd->border_width = cr.x;
		vd->ascent = cr.i;

		if (p->h < vd->border_height * 3 &&
		    (vd->border & (BORDER_TOP|BORDER_BOT)) ==
		    (BORDER_TOP|BORDER_BOT)) {
			vd->border &= ~BORDER_TOP;
			vd->border &= ~BORDER_BOT;
		}
		if (p->w < vd->border_width * 3 &&
		    (vd->border & (BORDER_LEFT|BORDER_RIGHT)) ==
		    (BORDER_LEFT|BORDER_RIGHT)) {
			vd->border &= ~BORDER_LEFT;
			vd->border &= ~BORDER_RIGHT;
		}

	}

	if (vd->border & BORDER_LEFT) {
		/* Left border is (currently) always a scroll bar */
		for (i = 0; i < p->h; i += vd->line_height)
			one_char(p, "|", "inverse", 0, i + vd->ascent);

		if (p->h > 4 * vd->line_height) {
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
			mid = vd->line_height + (p->h - 4 * vd->line_height) * ln / l;
			one_char(p, "^", 0, 0, mid-vd->line_height + vd->ascent);
			one_char(p, "#", "inverse", 0, mid + vd->ascent);
			one_char(p, "v", 0, 0, mid+vd->line_height + vd->ascent);
			one_char(p, "+", "inverse", 0, p->h
				  - vd->line_height + vd->ascent);
			vd->scroll_bar_y = mid;
		}
	}
	if (vd->border & BORDER_RIGHT) {
		for (i = 0; i < p->h; i += vd->line_height)
			one_char(p, "|", "inverse", p->w-vd->border_width,
				  i + vd->ascent);
	}
	if (vd->border & (BORDER_TOP | BORDER_BOT)) {
		name = pane_attr_get(p, "doc:name");
	}
	if (vd->border & BORDER_TOP) {
		int label;
		for (i = 0; i < p->w; i += vd->border_width)
			one_char(p, "-", "inverse", i, vd->ascent);
		snprintf(msg, sizeof(msg), "%s", name);
		label = (p->w - strlen(msg) * vd->border_width) / 2;
		if (label < vd->border_width)
			label = 1;
		one_char(p, msg, "inverse",
			 label * vd->border_width, vd->ascent);
	}
	if (vd->border & BORDER_BOT) {
		for (i = 0; i < p->w; i+= vd->border_width)
			one_char(p, "=", "inverse", i,
				  p->h-vd->border_height+vd->ascent);

		if (!(vd->border & BORDER_TOP)) {
			if (c >= 0)
				snprintf(msg, sizeof(msg), "L%d W%d C%d D:%s",
					 l,w,c, name);
			else
				snprintf(msg, sizeof(msg),"%s", name);
			one_char(p, msg, "inverse",
				 4*vd->border_width,
				 p->h-vd->border_height + vd->ascent);
		}
	}
	if (!(~vd->border & (BORDER_LEFT|BORDER_BOT)))
		/* Both are set */
		one_char(p, "+", "inverse", 0, p->h-vd->border_height+vd->ascent);
	if (!(~vd->border & (BORDER_RIGHT|BORDER_TOP)))
		one_char(p, "X", "inverse", p->w-vd->border_width, vd->ascent);
	if (!(~vd->border & (BORDER_LEFT|BORDER_TOP)))
		one_char(p, "/", "inverse", 0, vd->ascent);
	if (!(~vd->border & (BORDER_RIGHT|BORDER_BOT)))
		one_char(p, "/", "inverse", p->w-vd->border_width, p->h-vd->border_height+vd->ascent);
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
		free(vd);
		return 1;
	}
	if (strcmp(ci->key, "Clone") == 0) {
		struct pane *parent = ci->focus;
		struct pane *p2, *c;

		p2 = do_view_attach(parent, vd->old_border);
		c = pane_child(pane_child(p));
		if (c)
			return pane_clone(c, pane_final_child(p2));
		return 1;
	}
	if (strcmp(ci->key, "Refresh") == 0)
		return view_refresh(ci);
	if (strcmp(ci->key, "Notify:Replace") == 0) {
		pane_damaged(p, DAMAGED_CONTENT);
		return 1;
	}
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
				x += vd->border_width; w -= vd->border_width;
			}
			if (vd->border & BORDER_RIGHT) {
				w -= vd->border_width;
			}
			if (vd->border & BORDER_TOP) {
				y += vd->border_height; h -= vd->border_height;
			}
			if (vd->border & BORDER_BOT) {
				h -= vd->border_height;
			}
			pane_resize(p, x, y, w, h);
		}
		return 1;
	}
	return 0;
}

static struct pane *view_reattach(struct pane *par)
{
	struct view_data *vd = par->data;
	struct pane *p;

	call3("Request:Notify:Replace", par, 0, NULL);
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
	vd->old_border = border;
	vd->line_height = -1;
	vd->border_width = vd->border_height = -1;
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
	int lh = vd->line_height;
	char *key;
	int num;
	int cihx, cihy;

	cihx = ci->x; cihy = ci->y;
	pane_map_xy(ci->focus, ci->home, &cihx, &cihy);

	if (cihx >= vd->border_width)
		return 0;
	if (p->h <= 4)
		return 0;

	p = pane_child(p);

	key = "Move-View-Small";
	num = RPT_NUM(ci);

	if (cihy < mid - lh) {
		/* big scroll up */
		num = -num;
		key = "Move-View-Large";
	} else if (cihy <= mid) {
		/* scroll up */
		num = -num;
	} else if (cihy <= mid + lh) {
		/* scroll down */
	} else {
		/* big scroll down */
		key = "Move-View-Large";
	}
	return call3(key, pane_final_child(p), num, NULL);
}

DEF_CMD(view_border)
{
	struct pane *p = ci->home;
	struct view_data *vd = p->data;

	if (ci->numeric <= 0)
		vd->border = 0;
	else
		vd->border = vd->old_border;

	pane_damaged(p, DAMAGED_SIZE);
	return 0; /* allow other handlers to change borders */
}


void edlib_init(struct pane *ed)
{
	view_map = key_alloc();

	key_add(view_map, "Click-1", &view_click);
	key_add(view_map, "Press-1", &view_click);
	key_add(view_map, "Window:border", &view_border);

	call_comm("global-set-command", ed, 0, NULL, "attach-view",
		  0, &view_attach);
}
