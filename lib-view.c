/*
 * Copyright Neil Brown ©2015-2019 <neil@brown.name>
 * May be distributed under terms of GPLv2 - see file:COPYING
 *
 * A buffer can be viewed in a pane.
 * The pane is (typically) a tile in a display.
 * As well as content from the buffer, a 'view' provides
 * a scroll bar and a status line.
 * These serve to visually separate different views from each other.
 *
 */

#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>
#include <stdio.h>

#include "core.h"
#include "misc.h"

struct view_data {
	int		border;
	int		old_border;
	int		border_width, border_height;
	int		line_height;
	int		ascent;
	int		scroll_bar_y;
	struct mark	*viewpoint;
	struct pane	*child;

	int		move_small, move_large;
};
/* 0 to 4 borders are possible */
enum {
	BORDER_LEFT	= 1,
	BORDER_RIGHT	= 2,
	BORDER_TOP	= 4,
	BORDER_BOT	= 8,
	BORDER_STATUS	= 16, // override
};

static struct map *view_map safe;
DEF_LOOKUP_CMD(view_handle, view_map);
static struct pane *safe do_view_attach(struct pane *par, int border);
static int calc_border(struct pane *p safe);

static char default_status[] =
"{!CountLines}M:{doc-modified?,*,-}{doc-readonly?,%%,  } D:{doc-file-changed?,CHANGED:,}{doc-name:-15} L{^line}/{lines} {doc-status}";
static char default_title[] =
"{doc-name}";

static char *format_status(char *status safe,
			   struct pane *focus safe,
			   struct mark *pm)
{
	struct buf b;
	char *close;
	char *f;
	char type;
	int width;
	char *attr;
	char sep;

	buf_init(&b);
	while (*status) {
		int point_attr = 0;
		int l;

		if (*status != '{') {
			buf_append(&b, *status);
			status++;
			continue;
		}
		status += 1;
		close = strchr(status, '}');
		if (!close)
			break;
		f = strnsave(focus, status, close-status);
		if (!f)
			break;
		status = close + 1;

		if (*f == '!' && pm) {
			/* This is a command to call */
			call(f+1, focus, 0, pm);
			continue;
		}
		if (*f == '^') {
			point_attr = 1;
			f += 1;
		}
		/* Some extras here for future expansion */
		l = strcspn(f, ":+?#!@$%^&*=<>");
		type = f[l];
		f[l] = 0;
		if (point_attr && pm)
			attr = attr_find(*mark_attr(pm), f);
		else
			attr = pane_attr_get(focus, f);
		if (!attr)
			attr = "";
		switch (type) {
		case ':':
			/* Format in a field */
			width = atoi(f+l+1);
			if (width < 0) {
				buf_concat(&b, attr);
				width += strlen(attr);
				while (width < 0) {
					buf_append(&b, ' ');
					width += 1;
				}
			} else {
				width -= strlen(attr);
				while (width > 0) {
					buf_append(&b, ' ');
					width -= 1;
				}
				buf_concat(&b, attr);
			}
			break;
		case '?':
			/* flag - no, 0, empty, false are second option */
			f += l+1;
			sep = *f++;
			if (!sep)
				break;
			if (strcasecmp(attr, "no") == 0 ||
			    strcasecmp(attr, "false") == 0 ||
			    strcmp(attr, "0") == 0 ||
			    strlen(attr) == 0) {
				f = strchr(f, sep);
				if (f)
					f += 1;
				else
					f = "";
			}
			while (*f && *f != sep) {
				buf_append(&b, *f);
				f += 1;
			}
			break;
		default:
			buf_concat(&b, attr);
		}
	}
	return buf_final(&b);
}

static void one_char(struct pane *p safe, char *s, char *attr, int x, int y)
{
	call("Draw:text", p, -1, NULL, s, 0, NULL, attr, x, y);
}

DEF_CMD(view_refresh)
{
	struct pane *p = ci->home;
	struct view_data *vd = p->data;
	int i;
	struct mark *pm;
	char *status;
	char *title;

	if (vd->border <= 0)
		return 0;
	if (vd->line_height <= 0)
		return 0;

	pm = call_ret(mark, "doc:point", ci->focus);
	status = pane_attr_get(ci->focus, "status-line");
	if (!status)
		status = default_status;
	status = format_status(status, ci->focus, pm);
	title = pane_attr_get(ci->focus, "pane-title");
	if (!title)
		title = default_title;
	title = format_status(title, ci->focus, pm);

	if (vd->border & BORDER_LEFT) {
		/* Left border is (currently) always a scroll bar */
		for (i = 0; i < p->h; i += vd->line_height)
			one_char(p, "┃", "inverse", 0, i + vd->ascent);

		if (p->h > 4 * vd->line_height) {
			int l;
			int vpln = 0;
			int mid;

			if (vd->viewpoint) {
				call("CountLines", p, 0, vd->viewpoint);
				vpln = attr_find_int(*mark_attr(vd->viewpoint),
						     "line");
			} else if (pm) {
				call("CountLines", p, 0, pm);
				vpln = attr_find_int(*mark_attr(pm), "line");
			}

			l = pane_attr_get_int(ci->home, "lines");
			if (l <= 0)
				l = 1;
			mid = vd->line_height + (p->h - 4 * vd->line_height) * vpln / l;
			one_char(p, "^", NULL, 0, mid-vd->line_height + vd->ascent);
			one_char(p, "#", "inverse", 0, mid + vd->ascent);
			one_char(p, "v", NULL, 0, mid+vd->line_height + vd->ascent);
			one_char(p, "+", "inverse", 0, p->h
				  - vd->line_height + vd->ascent);
			vd->scroll_bar_y = mid;
		}
	}
	if (vd->border & BORDER_RIGHT) {
		for (i = 0; i < p->h; i += vd->line_height)
			one_char(p, "┃", "inverse", p->w-vd->border_width,
				  i + vd->ascent);
	}
	if (vd->border & BORDER_TOP) {
		int label;
		for (i = 0; i < p->w; i += vd->border_width)
			one_char(p, "━", "inverse", i, vd->ascent);
		label = (p->w - strlen(title?:"") * vd->border_width) / 2;
		if (label < vd->border_width)
			label = 1;
		one_char(p, title, "inverse",
			 label * vd->border_width, vd->ascent);
	}
	if (vd->border & BORDER_BOT) {
		for (i = 0; i < p->w; i+= vd->border_width)
			one_char(p, "═", "inverse", i,
				  p->h-vd->border_height+vd->ascent);

		if (!(vd->border & BORDER_TOP) ||
		    (vd->border & BORDER_STATUS)) {
			one_char(p, status, "inverse",
				 4*vd->border_width,
				 p->h-vd->border_height + vd->ascent);
		}
	}
	if (!(~vd->border & (BORDER_LEFT|BORDER_BOT)))
		/* Both are set */
		one_char(p, "┗", "inverse", 0, p->h-vd->border_height+vd->ascent);
	if (!(~vd->border & (BORDER_RIGHT|BORDER_TOP)))
		one_char(p, "╳", "inverse", p->w-vd->border_width, vd->ascent);
	if (!(~vd->border & (BORDER_LEFT|BORDER_TOP)))
		one_char(p, "┏", "inverse", 0, vd->ascent);
	if (!(~vd->border & (BORDER_RIGHT|BORDER_BOT)))
		one_char(p, "┛", "inverse", p->w-vd->border_width, p->h-vd->border_height+vd->ascent);

	free(status);
	free(title);

	return 0;
}

DEF_CMD(view_close)
{
	struct view_data *vd = ci->home->data;

	if (vd->viewpoint)
		mark_free(vd->viewpoint);
	return 1;
}

DEF_CMD(view_clone)
{
	struct view_data *vd = ci->home->data;
	struct pane *parent = ci->focus;
	struct pane *p2;

	p2 = do_view_attach(parent, vd->old_border);
	pane_clone_children(ci->home, p2);
	return 1;
}

DEF_CMD(view_child_registered)
{
	struct pane *p = ci->home;
	struct view_data *vd = p->data;
	vd->child = ci->focus;
	pane_damaged(p, DAMAGED_SIZE|DAMAGED_CONTENT);
	return 1;
}

DEF_CMD(view_refresh_size)
{
	struct pane *p = ci->home;
	struct view_data *vd = p->data;
	int x = 0, y = 0;
	int w = p->w;
	int h = p->h;
	int b;

	call("pane-clear", p);
	if (vd->border >= 0)
		vd->border = calc_border(ci->focus);
	b = vd->border < 0 ? 0 : vd->border;
	if (vd->line_height < 0) {
		struct call_return cr = call_ret(all, "text-size", ci->home,
						 -1, NULL, "M",
						 0, NULL, "bold");
		if (cr.ret == 0) {
			cr.x = cr.y =1;
			cr.i2 = 0;
		}
		vd->line_height = cr.y;
		vd->border_height = cr.y;
		vd->border_width = cr.x;
		vd->ascent = cr.i2;

		if (h < vd->border_height * 3 &&
		    (b & (BORDER_TOP|BORDER_BOT)) ==
		    (BORDER_TOP|BORDER_BOT)) {
			b &= ~BORDER_TOP;
			b &= ~BORDER_BOT;
		}
		if (w < vd->border_width * 3 &&
		    (b & (BORDER_LEFT|BORDER_RIGHT)) ==
		    (BORDER_LEFT|BORDER_RIGHT)) {
			b &= ~BORDER_LEFT;
			b &= ~BORDER_RIGHT;
		}

	}

	if (b & BORDER_LEFT) {
		x += vd->border_width; w -= vd->border_width;
	}
	if (b & BORDER_RIGHT) {
		w -= vd->border_width;
	}
	if (b & BORDER_TOP) {
		y += vd->border_height; h -= vd->border_height;
	}
	if (b & BORDER_BOT) {
		h -= vd->border_height;
	}
	if (w <= 0)
		w = 1;
	if (h <= 0)
		h = 1;
	if (vd->child)
		pane_resize(vd->child, x, y, w, h);

	return 1;
}

DEF_CMD(view_status_changed)
{
	pane_damaged(ci->home, DAMAGED_CONTENT);
	return 1;
}

DEF_CMD(view_reposition)
{
	struct view_data *vd = ci->home->data;

	if (vd->viewpoint != ci->mark) {
		if (!vd->viewpoint || !ci->mark ||
		    !mark_same(vd->viewpoint, ci->mark))
			pane_damaged(ci->home, DAMAGED_CONTENT);
		if (vd->viewpoint)
			mark_free(vd->viewpoint);
		if (ci->mark)
			vd->viewpoint = mark_dup(ci->mark);
		else
			vd->viewpoint = NULL;
	}
	return 0;
}

static struct pane *safe do_view_attach(struct pane *par, int border)
{
	struct view_data *vd;
	struct pane *p;

	vd = calloc(1, sizeof(*vd));
	vd->border = border;
	vd->old_border = border;
	vd->line_height = -1;
	vd->border_width = vd->border_height = -1;
	p = pane_register(par, 0, &view_handle.c, vd);
	/* Capture status-changed notification so we can update 'changed' flag in
	 * status line */
	call("doc:request:doc:status-changed", p);
	pane_damaged(p, DAMAGED_SIZE);
	return p;
}

static int calc_border(struct pane *p safe)
{
	int borders = 0;
	char *borderstr = pane_attr_get(p, "borders");
	if (!borderstr)
		borderstr = "";
	if (strchr(borderstr, 'T')) borders |= BORDER_TOP;
	if (strchr(borderstr, 'B')) borders |= BORDER_BOT;
	if (strchr(borderstr, 'L')) borders |= BORDER_LEFT;
	if (strchr(borderstr, 'R')) borders |= BORDER_RIGHT;
	if (strchr(borderstr, 's')) borders |= BORDER_STATUS;
	return borders;
}

DEF_CMD(view_attach)
{
	int borders = calc_border(ci->focus);

	return comm_call(ci->comm2, "callback:attach",
			 do_view_attach(ci->focus, borders));
}

DEF_CMD(view_click)
{
	struct pane *p = ci->home;
	struct view_data *vd = p->data;
	int mid = vd->scroll_bar_y;
	int lh = vd->line_height;
	int *size;
	int num;
	short cihx, cihy;

	cihx = ci->x; cihy = ci->y;
	pane_map_xy(ci->focus, ci->home, &cihx, &cihy);

	if (cihx >= vd->border_width)
		return 0;
	if (p->h <= 4)
		return 0;

	size = &vd->move_small;
	num = RPT_NUM(ci);

	if (cihy < mid - lh) {
		/* big scroll up */
		num = -num;
		size = &vd->move_large;
	} else if (cihy <= mid) {
		/* scroll up */
		num = -num;
	} else if (cihy <= mid + lh) {
		/* scroll down */
	} else {
		/* big scroll down */
		size = &vd->move_large;
	}
	*size += num;
	pane_damaged(p, DAMAGED_VIEW);
	return 1;
}

DEF_CMD(view_scroll)
{
	struct view_data *vd = ci->home->data;

	if (ci->key[6] == '4')
		vd->move_small -= 2;
	else
		vd->move_small += 2;
	pane_damaged(ci->home, DAMAGED_VIEW);
	return 1;
}

DEF_CMD(view_refresh_view)
{
	struct pane *p = ci->home;
	struct view_data *vd = p->data;
	int border;

	if (vd->move_large) {
		call("Move-View-Large", ci->focus, vd->move_large);
		vd->move_large = 0;
	}
	if (vd->move_small) {
		call("Move-View-Small", ci->focus, vd->move_small);
		vd->move_small = 0;
	}

	border = calc_border(ci->focus);
	if (vd->border >= 0 && border != vd->border) {
		vd->border = border;
		pane_damaged(p, DAMAGED_SIZE);
	}
	return 0;
}

DEF_CMD(view_clip)
{
	struct view_data *vd = ci->home->data;

	if (vd->viewpoint)
		mark_clip(vd->viewpoint, ci->mark, ci->mark2);
	return 0;
}

DEF_CMD(view_border)
{
	struct pane *p = ci->home;
	struct view_data *vd = p->data;

	if (ci->num <= 0)
		vd->border = -1;
	else
		vd->border = vd->old_border;

	pane_damaged(p, DAMAGED_SIZE);
	return 0; /* allow other handlers to change borders */
}

void edlib_init(struct pane *ed safe)
{
	view_map = key_alloc();

	key_add(view_map, "Click-1", &view_click);
	key_add(view_map, "Press-1", &view_click);
	key_add(view_map, "Press-4", &view_scroll);
	key_add(view_map, "Press-5", &view_scroll);
	key_add(view_map, "Window:border", &view_border);
	key_add(view_map, "Refresh:view", &view_refresh_view);
	key_add(view_map, "Close", &view_close);
	key_add(view_map, "Free", &edlib_do_free);
	key_add(view_map, "Clone", &view_clone);
	key_add(view_map, "ChildRegistered", &view_child_registered);
	key_add(view_map, "Refresh:size", &view_refresh_size);
	key_add(view_map, "Refresh", &view_refresh);
	key_add(view_map, "doc:status-changed", &view_status_changed);
	key_add(view_map, "render:reposition", &view_reposition);
	key_add(view_map, "Notify:clip", &view_clip);

	call_comm("global-set-command", ed, &view_attach, 0, NULL, "attach-view");
}
