/*
 * Copyright Neil Brown <neil@brown.name> 2015
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
};

static void popup_resize(struct pane *p, char *style)
{
	int x,y,w,h;
	/* First find the size */
	if (strchr(style, 'M'))
		h = p->parent->h/2 + 1;
	else
		h = 3;
	w = p->parent->w/2;
	if (strchr(style, '1')) w = (p->parent->w-2)/4 + 1;
	if (strchr(style, '3')) w = 3 * (p->parent->w-2)/4;
	if (strchr(style, '4')) w = p->parent->w-2;
	/* Now position */
	x = p->parent->w/2 - w/2 - 1;
	y = p->parent->h/2 - h/2 - 1;
	if (strchr(style, 'T')) { y = 0; h -= 1; }
	if (strchr(style, 'B')) { h -= 1; y = p->parent->h - h; }
	if (strchr(style, 'L')) x = 0;
	if (strchr(style, 'R')) x = p->parent->w - w;
	pane_resize(p, x, y, w, h);
}

static int do_popup_handle(struct command *c, struct cmd_info *ci)
{
	struct pane *p = ci->home;
	struct popup_info *ppi = p->data;

	if (strcmp(ci->key, "Close") == 0) {
		if (ppi->doc)
			doc_destroy(ppi->doc);
		free(ppi);
		/* FIXME : drop reference on ppi->doc ?? */
		return 1;
	}

	if (strcmp(ci->key, "Refresh") == 0) {
		popup_resize(p, ppi->style);
		return 0;
	}
	if (strcmp(ci->key, "popup:Abort") == 0) {
		pane_focus(ppi->target);
		pane_close(ppi->popup);
		return 1;
	}
	if (strcmp(ci->key, "popup:Replace") == 0) {
		struct cmd_info ci2 = {0};
		if (ci->str == NULL || ci->str[0] != '\n')
			return 0;

		pane_focus(ppi->target);
		ci2.focus = ppi->target;
		ci2.key = pane_attr_get(ci->focus, "done-key");
		if (!ci2.key)
			ci2.key = "PopupDone";
		ci2.numeric = 1;
		ci2.str = doc_getstr(ppi->doc, NULL, NULL);
		ci2.mark = NULL;
		key_handle_focus(&ci2);
		free(ci2.str);
		pane_close(ppi->popup);
		return 1;
	}
	if (strcmp(ci->key, "popup:get-target") == 0) {
		ci->focus = ppi->target;
		return 1;
	}
	return 0;
}
DEF_CMD(popup_handle, do_popup_handle);

static int popup_quote(struct command *c, struct cmd_info *ci)
{
	struct cmd_info ci2 = *ci;

	if (strcmp(ci->key, "Replace") == 0)
		ci2.key = "popup:Replace";
	else
		ci2.key = "popup:Abort";
	return key_handle_focus(&ci2);
}
DEF_CMD(comm_quote, popup_quote);

static int popup_attach(struct command *c, struct cmd_info *ci)
{
	/* attach a popup.  It can be attach to the view or the display,
	 * can be in a corner, in a side, or central, and be 1 line or
	 * multi line, and can have controlled width.
	 * These are set with individual character in ci->str as follows.
	 * D  - attach to display, otherwise is on focus.
	 * TBLR - 0, 1, or 2 can be given for center, side, or corner
	 * M  - multi line, else one line
	 * 1234 - how many quarters of width to use.(default 2);
	 */
	struct pane *root, *p;
	struct popup_info *ppi = malloc(sizeof(*ppi));
	struct point *pt;
	char *style = ci->str;
	char border[4];
	struct cmd_info ci2={0};
	int i, j;

	if (!style)
		style = "D3";
	if (strchr(style, 'D')) {
		struct cmd_info ci2 = {0};
		ci2.key = "global-key-root";
		ci2.focus = ci->focus;
		if (!key_handle_focus(&ci2))
			return 0;
		root = ci2.focus;
	} else {
		p = ci->focus;
		while (p && !p->point)
			p = p->parent;
		if (!p || !p->parent)
			return 0;
		root = p->parent;
	}

	ppi->target = ci->focus;
	ppi->popup = pane_register(root, 1, &popup_handle, ppi, NULL);
	ppi->style = style;
	popup_resize(ppi->popup, style);
	for (i = 0, j = 0; i < 4; i++) {
		if (strchr(style, "TLBR"[i]) == NULL)
			border[j++] = "TLBR"[i];
	}
	border[j] = 0;
	attr_set_str(&ppi->popup->attrs, "borders", border, -1);

	pt = doc_new(pane2ed(root), "text");
	doc_set_name(pt->doc, "*popup*");
	ppi->doc = pt->doc;
	p = pane_attach(ppi->popup, "view", pt, NULL);
	render_attach(NULL, p);
	pane_focus(p);
	ci2.key = "local-set-key";
	ci2.focus = p;
	ci2.str = "popup:quote";
	ci2.str2 = "Replace";
	key_handle_focus(&ci2);
	ci2.str2 = "Abort";
	key_handle_focus(&ci2);

	ci->home = p;
	return 1;
}
DEF_CMD(comm_attach, popup_attach);

void edlib_init(struct editor *ed)
{
	key_add(ed->commands, "attach-popup", &comm_attach);
	key_add(ed->commands, "popup:quote", &comm_quote);
}
