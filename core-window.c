/*
 * Copyright Neil Brown ©2023 <neil@brown.name>
 * May be distributed under terms of GPLv2 - see file:COPYING
 *
 * Core per-window functionality.
 *
 * Provide a pane that is instantiated between the root and any window
 * stack, to provide common functionality.  These includes:
 *
 * - setting per-window attributes
 * - registering and forwarding per-window notifications
 * - Being an intermediary for per-window selections.
 *
 * ==============================================================
 * Allow any pane to "claim ownership" of "the selection", or to
 * "commit" the selection.  A pane can also "discard" the selection,
 * but that only works if the pane owns it.
 *
 * This can be used for mouse-based copy/paste and interaction with the
 * X11 "PRIMARY" clipboard.
 * When a selection is made in any pane it claims "the selection".
 * When a mouse-based paste request is made, the receiving pane can ask for
 * the selection to be "commited", and then access the most recent copy-buffer.
 * The owner of a selection will, if the selection is still valid, call
 * copy:save to save the selected content.
 * When a "paste" request is made where the location is based on the "point"
 * (current cursor) it is unlikely that a selection in the same pane should be
 * used - if there is one it is more likely to be intended to receive the paste.
 * So the target pane can first "discard" the selection, then "commit", then call
 * "copy:get".  If the selection is in this pane, the "discard" will succeed,
 * the "commit" will be a no-op, and the top copy buf will be used.
 * If the selection is in another pane (or another app via X11), the "discard"
 * will fail (wrong owner), the "commit" will succeed and copy the selection,
 * and the "copy:get" will get it.
 *
 * Operations are "selection:claim", "selection:commit" and "selection:discard".
 * When the selection is claimed, the old owner gets called (not notified)
 * "Notify:selection:claimed", and when a commit request is made,
 * "Notify:selection:commit" is sent.
 *
 * A client can declare itself to be a fall-back handler by calling
 * select:claim with num==1.  Then if any other client discards its selection,
 * the ownership reverse to the fallback.  The fallback typically provides
 * access to some selection external to edlib, such as the x11 selections.
 */
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdio.h>

#define PANE_DATA_TYPE struct window_data
#include "core.h"
#include "internal.h"

struct window_data {
	struct pane	*sel_owner;
	int		sel_committed;
	struct pane	*sel_owner_fallback;
};
#include "core-pane.h"

DEF_CMD(request_notify)
{
	pane_add_notify(ci->focus, ci->home, ksuffix(ci, "window:request:"));
	return 1;
}

DEF_CMD(send_notify)
{
	/* window:notify:... */
	return home_pane_notify(ci->home, ksuffix(ci, "window:notify:"),
				ci->focus,
				ci->num, ci->mark, ci->str,
				ci->num2, ci->mark2, ci->str2, ci->comm2);
}

DEF_CMD(window_set)
{
	const char *val = ksuffix(ci, "window:set:");

	if (!*val)
		val = ci->str2;
	if (!val)
		return Enoarg;

	attr_set_str(&ci->home->attrs, val, ci->str);

	return 1;
}

DEF_CMD(selection_claim)
{
	struct window_data *wd = ci->home->data;

	if (wd->sel_owner && wd->sel_owner != ci->focus) {
		call("Notify:selection:claimed", wd->sel_owner);
		//pane_drop_notifiers(ci->home, "Notify:Close", wd->sel_owner);
	}
	wd->sel_owner = ci->focus;
	if (ci->num == 1)
		wd->sel_owner_fallback = ci->focus;
	wd->sel_committed = 0;
	pane_add_notify(ci->home, ci->focus, "Notify:Close");
	return 1;
}

DEF_CMD(selection_commit)
{
	struct window_data *wd = ci->home->data;

	if (wd->sel_owner && !wd->sel_committed) {
		if (call("Notify:selection:commit", wd->sel_owner) != 2)
			wd->sel_committed = 1;
	}
	return 1;
}

DEF_CMD(selection_discard)
{
	struct window_data *wd = ci->home->data;
	struct pane *op, *fp;

	if (!wd->sel_owner)
		return Efalse;
	if (wd->sel_owner_fallback == ci->focus)
		wd->sel_owner_fallback = NULL;
	/* Don't require exactly same pane for sel_owner,
	 * but ensure they have the same focus.
	 */
	op = pane_focus(wd->sel_owner);
	fp = pane_focus(ci->focus);
	if (fp != op)
		return Efalse;

	wd->sel_owner = wd->sel_owner_fallback;
	wd->sel_committed = 0;
	return 1;
}

DEF_CMD(scale_image)
{
	/* This is a helper for Draw:image which interprets str2
	 * with other values and calls comm2 with:
	 * "width" returns image width
	 * "height" returns image height
	 * "scale"  num=new width, num2=new height
	 * "crop" x,y is top-left num,num2 - width,height
	 *	    These numbers apply after scaling.
	 * "draw"  num,num2 = offset
	 * "cursor" x,y=pos, num,num2=size
	 *
	 * Inputs are:
	 * 'str2' container 'mode' information.
	 *     By default the image is placed centrally in the pane
	 *     and scaled to use either fully height or fully width.
	 *     Various letters modify this:
	 *     'S' - stretch to use full height *and* full width
	 *     'L' - place on left if full width isn't used
	 *     'R' - place on right if full width isn't used
	 *     'T' - place at top if full height isn't used
	 *     'B' - place at bottom if full height isn't used.
	 *
	 *    Also a suffix ":NNxNN" will be parse and the two numbers used
	 *    to give number of rows and cols to overlay on the image for
	 *    the purpose of cursor positioning.  If these are present and
	 *    p->cx,cy are not negative, draw a cursor at p->cx,cy highlighting
	 *    the relevant cell.
	 *
	 * num,num2, if both positive, override the automatic scaling.
	 *    The image is scaled to this many pixels.
	 * x,y is top-left pixel in the scaled image to start display at.
	 *    Negative values allow a margin between pane edge and this image.
	 */
	struct pane *p = ci->focus;
	const char *mode = ci->str2 ?: "";
	bool stretch = strchr(mode, 'S');
	int w, h;
	int x = 0, y = 0;
	int pw, ph;
	int xo = 0, yo = 0;
	int cix, ciy;
	const char *pxl;
	short px, py;

	if (!ci->comm2)
		return Enoarg;

	pxl = pane_attr_get(p, "Display:pixels");
	if (sscanf(pxl ?: "1x1", "%hdx%hx", &px, &py) != 2)
		px = py = 1;

	w = p->w * px;
	h = p->h * py;
	if (ci->num > 0 && ci->num2 > 0) {
		w = ci->num;
		h = ci->num2;
	} else if (ci->num > 0) {
		int iw = comm_call(ci->comm2, "width", p);
		int ih = comm_call(ci->comm2, "height", p);

		if (iw <= 0 || ih <= 0)
			return Efail;

		w = iw * ci->num / 1024;
		h = ih * ci->num / 1024;
	} else if (!stretch) {
		int iw = comm_call(ci->comm2, "width", p);
		int ih = comm_call(ci->comm2, "height", p);

		if (iw <= 0 || ih <= 0)
			return Efail;

		if (iw * h > ih * w) {
			/* Image is wider than space, use less height */
			ih = ih * w / iw;
			if (strchr(mode, 'B'))
				/* bottom */
				y = h - ih;
			else if (!strchr(mode, 'T'))
				/* center */
				y = (h - ih) / 2;
			/* Round up to pixels-per-cell */
			h = ((ih + py - 1) / py) * py;
		} else {
			/* image is too tall, use less width */
			iw = iw * h / ih;
			if (strchr(mode, 'R'))
				/* right */
				x = w - iw;
			else if (!strchr(mode, 'L'))
				x = (w - iw) / 2;
			w = ((iw + px - 1) / px) * px;
		}
	}

	comm_call(ci->comm2, "scale", p, w, NULL, NULL, h);
	pw = p->w * px;
	ph = p->h * py;
	cix = ci->x;
	ciy = ci->y;
	if (cix < 0) {
		xo -= cix;
		pw += cix;
		cix = 0;
	}
	if (ciy < 0) {
		yo -= ciy;
		ph += ciy;
		ciy = 0;
	}
	if (w - cix <= pw)
		w -= cix;
	else
		w = pw;
	if (h - ciy <= ph)
		h -= ciy;
	else
		h = ph;
	comm_call(ci->comm2, "crop", p, w, NULL, NULL, h, NULL, NULL, cix, ciy);
	comm_call(ci->comm2, "draw", p, x + xo, NULL, NULL, y + yo);

	if (p->cx >= 0) {
		int rows, cols;
		char *cl = strchr(mode, ':');
		if (cl && sscanf(cl, ":%dx%d", &cols, &rows) == 2)
			comm_call(ci->comm2, "cursor", p,
				  w/cols, NULL, NULL, h/rows, NULL, NULL,
				  p->cx + xo, p->cy + yo);
	}
	return 1;
}

DEF_CMD(window_activate_display)
{
	/* Given a display attached to the root, integrate it
	 * into a full initial stack of panes.
	 * The display is the focus of this pane.  This doc to
	 * attach there is the focus in the command.
	 */
	struct pane *disp = ci->home->focus;
	struct pane *p, *p2;
	bool display_added = False;
	char *ip;
	char *save, *t, *m;

	if (!disp || !list_empty(&disp->children))
		return Efail;
	ip = pane_attr_get(disp, "window-initial-panes");
	if (!ip)
		return Efail;
	ip = strdup(ip);
	p = ci->home;

	for (t = strtok_r(ip, " \t\n", &save);
	     t;
	     t = strtok_r(NULL, " \t\n", &save)) {
		if (!*t)
			continue;
		if (strcmp(t, "DISPLAY") == 0) {
			if (!display_added) {
				pane_reparent(disp, p);
				p = disp;
				display_added = True;
			}
		} else {
			m = strconcat(NULL, "attach-", t);
			p2 = call_ret(pane, m, p);
			free(m);
			if (p2)
				p = p2;
		}
	}
	free(ip);
	if (p && ci->focus != disp)
		p = home_call_ret(pane, ci->focus, "doc:attach-view", p, 1);
	if (p)
		comm_call(ci->comm2, "cb", p);
	return 1;
}

DEF_CMD(close_notify)
{
	struct window_data *wd = ci->home->data;

	if (wd->sel_owner_fallback == ci->focus)
		wd->sel_owner_fallback = NULL;

	if (wd->sel_owner == ci->focus)
		wd->sel_owner = wd->sel_owner_fallback;
	return 1;
}

static struct map *window_map;
DEF_LOOKUP_CMD(window_handle, window_map);

DEF_CMD(window_attach)
{
	struct pane *p;

	p = pane_register(pane_root(ci->focus), 0, &window_handle.c);
	if (!p)
		return Efail;
	comm_call(ci->comm2, "cb", p);
	return 1;
}

DEF_CMD(window_close)
{
	pane_close(ci->home);
	return 1;
}

void window_setup(struct pane *ed safe)
{
	window_map = key_alloc();

	key_add_prefix(window_map, "window:request:", &request_notify);
	key_add_prefix(window_map, "window:notify:", &send_notify);

	key_add(window_map, "window:close", &window_close);

	key_add_prefix(window_map, "window:set:", &window_set);

	key_add(window_map, "selection:claim", &selection_claim);
	key_add(window_map, "selection:commit", &selection_commit);
	key_add(window_map, "selection:discard", &selection_discard);
	key_add(window_map, "Notify:Close", &close_notify);

	key_add(window_map, "Draw:scale-image", &scale_image);
	key_add(window_map, "window:activate-display",
		&window_activate_display);

	call_comm("global-set-command", ed, &window_attach, 0, NULL,
		  "attach-window-core");
}
