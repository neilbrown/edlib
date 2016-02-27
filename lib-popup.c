/*
 * Copyright Neil Brown ©2015 <neil@brown.name>
 * May be distributed under terms of GPLv2 - see file:COPYING
 *
 * popup
 *
 * A 'popup' dialogue pane can be used to enter a file name or
 * probably lots of other things.
 * It gets a high 'z' value so it obscures whatever is behind.
 *
 * As well a interacting with its own buffer, a popup can pass events
 * on to other panes, and it can disappear.
 * For now these are combined - the <ENTER> key will make the window
 * disappear and will pass a message with the content of the text
 * as a string.
 * The target pane must not disappear while the popup is active.
 * I need to find a way to control that.
 *
 * A popup is created by "attach-popup"
 * A prefix to be displayed can be added by setting "prefix" on the popup pane.
 * The event sent when the popup is closed can be set by setting attribute "done-key"
 * otherwise "PopupDone" is used.
 */
#define _GNU_SOURCE /*  for asprintf */
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <memory.h>

#include "core.h"

struct popup_info {
	struct pane	*target, *popup;
	struct doc	*doc;
	char		*style;
	int closing;
};

DEF_CMD(text_size_callback)
{
	struct call_return *cr = container_of(ci->comm, struct call_return, c);
	cr->x = ci->x;
	cr->y = ci->y;
	cr->i = ci->numeric;
	cr->i2 = ci->extra;
	return 1;
}

static int line_height(struct pane *p)
{
	struct call_return cr;

	cr.c = text_size_callback;
	call_comm7("text-size", p, -1, NULL, "x", 0, "", &cr.c);
	return cr.y;
}

static void popup_resize(struct pane *p, char *style)
{
	int x,y,w,h;
	int lh;

	/* First find the size */
	lh = line_height(p);
	if (strchr(style, 'M'))
		h = p->parent->h/2 + 1;
	else
		h = lh * 3;
	w = p->parent->w/2;
	if (strchr(style, '1')) w = (p->parent->w-2)/4 + 1;
	if (strchr(style, '3')) w = 3 * (p->parent->w-2)/4;
	if (strchr(style, '4')) w = p->parent->w-2;
	/* Now position */
	x = p->parent->w/2 - w/2 - 1;
	y = p->parent->h/2 - h/2 - 1;
	if (strchr(style, 'T')) { y = 0; h -= lh; }
	if (strchr(style, 'B')) { h -= lh; y = p->parent->h - h; }
	if (strchr(style, 'L')) x = 0;
	if (strchr(style, 'R')) x = p->parent->w - w;
	pane_resize(p, x, y, w, h);
}

DEF_CMD(popup_handle)
{
	struct pane *p = ci->home;
	struct popup_info *ppi = p->data;
	struct doc *d;

	if (strcmp(ci->key, "Close") == 0) {
		d = ppi->doc; ppi->doc = NULL;
		if (d)
			/* FIXME make this doc auto-close */
			doc_destroy(d->home);
		free(ppi);
		return 1;
	}

	if (strcmp(ci->key, "Notify:Close") == 0) {
		if (ci->focus == ppi->target && !ppi->closing) {
			d = ppi->doc; ppi->doc = NULL;
			if (d)
				/* FIXME make this doc auto-close */
				doc_destroy(d->home);
			pane_close(p);
		}
		return 1;
	}

	if (strcmp(ci->key, "Refresh") == 0) {
		popup_resize(p, ppi->style);
		return 1;
	}
	if (strcmp(ci->key, "popup:Abort") == 0) {
		pane_focus(ppi->target);
		ppi->closing = 1;
		call3("Abort", ppi->target, 0, NULL);
		d = ppi->doc; ppi->doc = NULL;
		if (d)
			/* FIXME make this doc auto-close */
			doc_destroy(d->home);
		pane_close(ppi->popup);
		return 1;
	}
	if (strcmp(ci->key, "popup:Return") == 0) {
		struct cmd_info ci2 = {0};

		ppi->closing = 1;
		pane_focus(ppi->target);
		ci2.focus = ppi->target;
		ci2.key = pane_attr_get(ci->focus, "done-key");
		if (!ci2.key)
			ci2.key = "PopupDone";
		ci2.numeric = 1;
		ci2.str = ci->str;
		if (ppi->doc)
			ci2.str = doc_getstr(pane_final_child(ppi->popup), NULL);
		ci2.mark = NULL;
		key_handle(&ci2);
		if (ppi->doc)
			free(ci2.str);
		d = ppi->doc; ppi->doc = NULL;
		if (d)
			/* FIXME make this doc auto-close */
			doc_destroy(d->home);
		pane_close(ppi->popup);
		return 1;
	}
	if (strcmp(ci->key, "popup:get-target") == 0)
		return comm_call(ci->comm2, "callback:get-target",
				 ppi->target, 0, NULL, NULL, 0);

	return 0;
}

DEF_CMD(popup_quote)
{
	struct cmd_info ci2 = {0};

	if (strcmp(ci->key, "Return") == 0)
		ci2.key = "popup:Return";
	else
		ci2.key = "popup:Abort";
	ci2.focus = ci->focus;
	ci2.numeric = ci->numeric;
	ci2.extra = ci->extra;
	ci2.mark = ci->mark;
	return key_handle(&ci2);
}

DEF_CMD(popup_attach)
{
	/* attach a popup.  It can be attach to the view or the display,
	 * can be in a corner, in a side, or central, and be 1 line or
	 * multi line, and can have controlled width.
	 * These are set with individual character in ci->str as follows.
	 * D  - attach to display, otherwise is on focus.
	 * TBLR - 0, 1, or 2 can be given for center, side, or corner
	 * M  - multi line, else one line
	 * 1234 - how many quarters of width to use.(default 2);
	 * r  - allow recursive popup
	 */
	struct pane *root, *p;
	struct popup_info *ppi = malloc(sizeof(*ppi));
	char *style = ci->str;
	char border[4];
	struct cmd_info ci2={0};
	int i, j;
	int z = 1;

	if (!style)
		style = "D3";

	if (!strchr(style, 'r') &&
	    pane_attr_get(ci->focus, "Popup") != NULL)
		/* No recusive popups without permission */
		return 0;

	if (strchr(style, 'D')) {
		int x = 0, y = 0;
		pane_to_root(ci->focus, &x, &y, &z, NULL, NULL);
		root = call_pane("RootPane", ci->focus, 0, NULL, 0);
	} else
		root = call_pane("ThisPane", ci->focus, 0, NULL, 0);
	if (!root)
		return 0;

	ppi->target = ci->focus;
	ppi->popup = pane_register(root, z, &popup_handle, ppi, NULL);
	ppi->style = style;
	ppi->doc = NULL;
	ppi->closing = 0;
	popup_resize(ppi->popup, style);
	for (i = 0, j = 0; i < 4; i++) {
		if (strchr(style, "TLBR"[i]) == NULL)
			border[j++] = "TLBR"[i];
	}
	border[j] = 0;
	attr_set_str(&ppi->popup->attrs, "Popup", "true", -1);
	attr_set_str(&ppi->popup->attrs, "borders", border, -1);
	attr_set_str(&ppi->popup->attrs, "render-wrap", "no", -1);

	pane_add_notify(ppi->popup, ppi->target, "Notify:Close");

	if (ci->home) {
		p = doc_attach_view(ppi->popup, ci->home, NULL);
	} else {
		struct doc *d;
		d = doc_new(pane2ed(root), "text");
		doc_set_name(d, "*popup*");
		ppi->doc = d;
		p = doc_attach_view(ppi->popup, d->home, NULL);
	}
	pane_focus(p);

	ci2.key = "local-set-key";
	ci2.focus = p;
	ci2.str = "popup:quote";
	ci2.str2 = "Return";
	key_handle(&ci2);
	ci2.focus = pane_final_child(p);
	ci2.home = NULL; ci2.comm = NULL;
	ci2.str2 = "Abort";
	key_handle(&ci2);

	return comm_call(ci->comm2, "callback:attach", ppi->popup, 0, NULL, NULL, 0);
}

void edlib_init(struct editor *ed)
{
	key_add(ed->commands, "attach-popup", &popup_attach);
	key_add(ed->commands, "popup:quote", &popup_quote);
}
