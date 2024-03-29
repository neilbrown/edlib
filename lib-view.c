/*
 * Copyright Neil Brown ©2015-2023 <neil@brown.name>
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

#define PANE_DATA_TYPE struct view_data
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
};
#include "core-pane.h"

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
static struct pane *do_view_attach(struct pane *par safe, int border);
static int calc_border(struct pane *p safe);

static const char default_status[] =
	"{!CountLinesAsync}M:{doc-modified?,*,-}{doc-readonly?,%%,  } D:{doc-file-changed?,CHANGED:,}{doc-name%-15} L{^line}/{lines} {display-context}{render-default}/{view-default} {doc-status}";
static const char default_title[] =
	"{doc-name}";

static char *format_status(const char *status safe,
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
		case '%': /* make spaces visible */
			if (attr[0] <= ' ' ||
			    (attr[0] && attr[strlen(attr)-1] <= ' '))
				attr = strconcat(focus, "\"", attr, "\"");
			/* fallthrough */
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

static void one_char(struct pane *p safe, const char *s, char *attr, int x, int y)
{
	call("Draw:text", p, -1, NULL, s, 0, NULL, attr, x, y);
}

DEF_CMD(view_refresh)
{
	struct pane *p = ci->home;
	struct view_data *vd = p->data;
	int i;
	struct mark *pm;
	const char *status;
	const char *title;

	if (vd->border <= 0)
		return 1;
	if (vd->line_height <= 0)
		return 1;

	call("Draw:clear", p, 0, NULL, "bg:white");
	pm = call_ret(mark, "doc:point", ci->focus);
	status = pane_attr_get(ci->focus, "status-line");
	if (!status)
		status = default_status;
	status = format_status(status, ci->focus, pm);
	title = pane_attr_get(ci->focus, "pane-title");
	if (!title)
		title = default_title;
	title = format_status(title, ci->focus, pm);

	mark_watch(pm);

	if (vd->border & BORDER_LEFT) {
		/* Left border is (currently) always a scroll bar */
		for (i = 0; i < p->h; i += vd->line_height)
			one_char(p, "┃", "inverse", 0, i + vd->ascent);

		if (p->h > 4 * vd->line_height) {
			int l;
			int vpln = 0;
			int mid;

			if (vd->viewpoint) {
				call("CountLinesAsync", ci->focus, 0, vd->viewpoint);
				vpln = attr_find_int(*mark_attr(vd->viewpoint),
						     "line");
			} else if (pm) {
				call("CountLinesAsync", ci->focus, 0, pm);
				vpln = attr_find_int(*mark_attr(pm), "line");
			}

			l = pane_attr_get_int(ci->focus, "lines", 1);
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
			 label, vd->ascent);
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

	return 1;
}

DEF_CMD_CLOSED(view_close)
{
	struct view_data *vd = ci->home->data;

	mark_free(vd->viewpoint);
	vd->viewpoint = NULL;
	return 1;
}

DEF_CMD(view_clone)
{
	struct view_data *vd = ci->home->data;
	struct pane *parent = ci->focus;
	struct pane *p2;

	p2 = do_view_attach(parent, vd->old_border);
	if (p2)
		pane_clone_children(ci->home, p2);
	return 1;
}

DEF_CMD(view_child_notify)
{
	struct pane *p = ci->home;
	struct view_data *vd = p->data;

	if (ci->focus->z)
		return 1;
	if (vd->child && ci->num > 0)
		pane_close(vd->child);
	if (ci->num > 0)
		vd->child = ci->focus;
	else if (vd->child == ci->focus)
		vd->child = NULL;
	p->focus = vd->child;
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

	if (vd->border >= 0)
		vd->border = calc_border(ci->focus);
	b = vd->border < 0 ? 0 : vd->border;
	if (vd->line_height < 0) {
		/* FIXME should use scale */
		struct call_return cr = call_ret(all, "Draw:text-size", ci->home,
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
		attr_set_int(&p->attrs, "border-width", cr.x);
		attr_set_int(&p->attrs, "border-height", cr.y);
#if 0
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
#endif
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
	pane_damaged(p, DAMAGED_REFRESH);
	if (vd->child)
		pane_resize(vd->child, x, y, w, h);

	return 1;
}

DEF_CMD(view_status_changed)
{
	if (strcmp(ci->key, "mark:moving") == 0) {
		struct mark *pt = call_ret(mark, "doc:point", ci->home);
		if (pt != ci->mark)
			return 1;
	}
	pane_damaged(ci->home, DAMAGED_VIEW);
	pane_damaged(ci->home, DAMAGED_REFRESH);
	if (strcmp(ci->key, "view:changed") == 0)
		return Efallthrough;
	return 1;
}

DEF_CMD(view_reposition)
{
	struct view_data *vd = ci->home->data;

	if (!ci->mark)
		return Efallthrough;

	if (!vd->viewpoint || !mark_same(vd->viewpoint, ci->mark)) {
		pane_damaged(ci->home, DAMAGED_REFRESH);
		if (vd->viewpoint)
			mark_free(vd->viewpoint);
		if (ci->mark)
			vd->viewpoint = mark_dup(ci->mark);
		else
			vd->viewpoint = NULL;
	}
	return Efallthrough;
}

static struct pane *do_view_attach(struct pane *par safe, int border)
{
	struct view_data *vd;
	struct pane *p;

	p = pane_register(par, 0, &view_handle.c);
	if (!p)
		return p;
	vd = p->data;
	vd->border = border;
	vd->old_border = border;
	vd->line_height = -1;
	vd->border_width = vd->border_height = -1;
	/* Capture status-changed notification so we can update 'changed' flag in
	 * status line */
	call("doc:request:doc:status-changed", p);
	call("doc:request:doc:replaced", p);
	call("doc:request:mark:moving", p);
	/* And update display-context */
	call("Window:request:display-context", p);
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
	struct pane *p = do_view_attach(ci->focus, borders);

	if (!p)
		return Efail;
	return comm_call(ci->comm2, "callback:attach", p);
}

DEF_CMD(view_click)
{
	struct pane *p = ci->home;
	struct view_data *vd = p->data;
	struct pane *c = vd->child;
	int mid = vd->scroll_bar_y;
	int lh = vd->line_height;
	int num;
	int scale;
	struct xy cih;

	cih = pane_mapxy(ci->focus, ci->home, ci->x, ci->y, False);

	if (ci->focus != p)
		/* Event was in the child */
		return Efallthrough;
	if (!c)
		return 1;
	/* Ignore if not in scroll-bar, which it to left of child */
	if (cih.y < c->y ||		// above child
	    cih.y >= c->y + c->h ||	// below child
	    cih.x >= c->x)		// Not to right of child
		return 1;

	if (p->h <= 4)
		/* scroll bar too small to be useful */
		return 1;

	scale = 100; /* 10% for small movements */
	num = RPT_NUM(ci);

	if (cih.y < mid - lh) {
		/* big scroll up */
		num = -num;
		scale = 900;
	} else if (cih.y <= mid) {
		/* scroll up */
		num = -num;
	} else if (cih.y <= mid + lh) {
		/* scroll down */
	} else {
		/* big scroll down */
		scale = 900;
	}
	call("Move-View", pane_focus(ci->focus), num * scale);
	return 1;
}

DEF_CMD(view_release)
{
	/* Make sure release doesn't go to parent if not in child */

	if (ci->focus != ci->home)
		/* Event was in the child */
		return Efallthrough;
	return 1;
}

DEF_CMD(view_scroll)
{
	if (strcmp(ci->key, "M:Press-4") == 0)
		call("Move-View", pane_focus(ci->focus), -200);
	else
		call("Move-View", pane_focus(ci->focus), 200);
	return 1;
}

DEF_CMD(view_refresh_view)
{
	struct pane *p = ci->home;
	struct view_data *vd = p->data;
	int border;

	border = calc_border(ci->focus);
	if (vd->border >= 0 && border != vd->border) {
		vd->border = border;
		pane_damaged(p, DAMAGED_SIZE);
	}
	return 1;
}

DEF_CMD(view_clip)
{
	struct view_data *vd = ci->home->data;

	if (vd->viewpoint)
		mark_clip(vd->viewpoint, ci->mark, ci->mark2, !!ci->num);
	return Efallthrough;
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
	return Efallthrough; /* allow other handlers to change borders */
}

void edlib_init(struct pane *ed safe)
{
	view_map = key_alloc();

	key_add(view_map, "M:Click-1", &view_click);
	key_add(view_map, "M:Press-1", &view_click);
	key_add(view_map, "M:Release-1", &view_release);
	key_add(view_map, "M:DPress-1", &view_click);
	key_add(view_map, "M:TPress-1", &view_click);
	key_add(view_map, "M:Press-4", &view_scroll);
	key_add(view_map, "M:Press-5", &view_scroll);
	key_add(view_map, "Tile:border", &view_border);
	key_add(view_map, "Refresh:view", &view_refresh_view);
	key_add(view_map, "Close", &view_close);
	key_add(view_map, "Clone", &view_clone);
	key_add(view_map, "Child-Notify", &view_child_notify);
	key_add(view_map, "Refresh:size", &view_refresh_size);
	key_add(view_map, "Refresh", &view_refresh);
	key_add(view_map, "doc:status-changed", &view_status_changed);
	key_add(view_map, "doc:replaced", &view_status_changed);
	key_add(view_map, "mark:moving", &view_status_changed);
	key_add(view_map, "view:changed", &view_status_changed);
	key_add(view_map, "display-context", &view_status_changed);
	key_add(view_map, "render:reposition", &view_reposition);
	key_add(view_map, "Notify:clip", &view_clip);

	call_comm("global-set-command", ed, &view_attach, 0, NULL, "attach-view");
}
