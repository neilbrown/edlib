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
 * A popup is created by "PopupTile"
 * A prefix to be displayed can be added by setting "prefix" on the popup pane.
 * A default value can be given with attr "default" which is displated after prefix
 * The event sent when the popup is closed can be set by setting attribute "done-key"
 * otherwise "PopupDone" is used.
 */
#define _GNU_SOURCE /*  for asprintf */
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <memory.h>

#include "core.h"

static struct map *popup_map;
DEF_LOOKUP_CMD(popup_handle, popup_map);

struct popup_info {
	struct pane	*target safe, *popup safe, *handle safe;
	char		*style safe;
};

static int line_height(struct pane *p safe)
{
	struct call_return cr =
		call_ret(all, "text-size", p, -1, NULL, "x",
			 0, NULL, "");
	return cr.y;
}

static void popup_resize(struct pane *p safe, char *style safe)
{
	int x,y,w,h;
	int lh;

	if (!p->parent)
		/*FIXME impossible */
		return;
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

DEF_CMD(popup_close)
{
	struct popup_info *ppi = ci->home->data;
	free(ppi->style);
	free(ppi);
	return 1;
}

DEF_CMD(popup_notify_close)
{
	struct popup_info *ppi = ci->home->data;

	if (ci->focus == ppi->target) {
		/* target is closing, so we close too */
		ppi->target = safe_cast NULL;
		pane_close(ci->home);
	}
	return 1;
}

DEF_CMD(popup_abort)
{
	struct popup_info *ppi = ci->home->data;

	pane_focus(ppi->target);
	call("Abort", ppi->target);
	pane_close(ppi->popup);
	return 1;
}

DEF_CMD(popup_style)
{
	struct popup_info *ppi = ci->home->data;
	char border[5];
	int i, j;;

	if (!ci->str)
		return 0;

	free(ppi->style);
	ppi->style = strdup(ci->str);
	for (i = 0, j = 0; i < 4; i++) {
		if (strchr(ppi->style, "TLBR"[i]) == NULL)
			border[j++] = "TLBR"[i];
	}
	border[j] = 0;
	attr_set_str(&ppi->popup->attrs, "Popup", "true");
	attr_set_str(&ppi->popup->attrs, "borders", border);
	popup_resize(ci->home, ppi->style);
	return 1;
}

DEF_CMD(popup_refresh_size)
{
	struct popup_info *ppi = ci->home->data;
	char *prompt, *dflt, *prefix;

	prefix = attr_find(ppi->handle->attrs, "prefix");
	prompt = attr_find(ppi->handle->attrs, "prompt");
	if (!prefix && prompt) {
		char *t = NULL;
		dflt = attr_find(ppi->handle->attrs, "default");
		if (!prompt)
			prompt = "";
		if (dflt)
			asprintf(&t, "%s(%s): ", prompt, dflt);
		else
			asprintf(&t, "%s: ", prompt);
		attr_set_str(&ppi->handle->attrs, "prefix", t);
		free(t);
	}

	popup_resize(ci->home, ppi->style);
	return 0;
}

DEF_CMD(popup_get_target)
{
	struct popup_info *ppi = ci->home->data;
	return comm_call(ci->comm2, "callback:get-target", ppi->target);
}

DEF_CMD(popup_do_close)
{
	struct popup_info *ppi = ci->home->data;
	char *key, *str;
	struct pane *target = ppi->target;

	pane_focus(target);
	key = pane_attr_get(ci->focus, "done-key");
	if (!key)
		key = "PopupDone";
	str = ci->str;
	if (!str || !str[0])
		str = pane_attr_get(ci->focus, "default");
	pane_close(ppi->popup);
	/* This pane is closed now, ppi is gone. Be careful */
	call(key, target, 1, NULL, str);
	return 1;
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
	struct popup_info *ppi;
	char *style = ci->str;
	char border[5];
	int i, j;
	int z;

	if (!style)
		style = "D3";

	if (!strchr(style, 'r') &&
	    pane_attr_get(ci->focus, "Popup") != NULL)
		/* No recusive popups without permission */
		return 0;

	if (strchr(style, 'D')) {
		root = call_pane("RootPane", ci->focus);
	} else
		root = call_pane("ThisPane", ci->focus);
	if (!root)
		return 0;

	ppi = malloc(sizeof(*ppi));
	ppi->target = ci->focus;
	/* HACK this is because of +1 in pane_do_resize */
	z = ci->focus->abs_z - root->abs_z;
	if (z < 0)
		z = 1;

	ppi->popup = p = pane_register(root, z + 1, &popup_handle.c, ppi, NULL);
	ppi->style = strdup(style);
	popup_resize(ppi->popup, style);
	for (i = 0, j = 0; i < 4; i++) {
		if (strchr(style, "TLBR"[i]) == NULL)
			border[j++] = "TLBR"[i];
	}
	border[j] = 0;
	attr_set_str(&ppi->popup->attrs, "Popup", "true");
	attr_set_str(&ppi->popup->attrs, "borders", border);
	attr_set_str(&ppi->popup->attrs, "render-wrap", "no");

	pane_add_notify(ppi->popup, ppi->target, "Notify:Close");
	pane_focus(ppi->popup);

	if (ci->str2) {
		struct pane *doc =
			call_pane("doc:from-text", ppi->popup, 0, NULL,
				  "*popup*", 0, NULL, ci->str2);
		if (doc &&
		    (p = doc_attach_view(ppi->popup, doc, NULL)) != NULL) {

			call("Move-File", p, 1);
			call("doc:set:autoclose", p, 1);
		}
	}

	if (!p)
		return -1;
	ppi->handle = p;
	return comm_call(ci->comm2, "callback:attach", p);
}

void edlib_init(struct pane *ed safe)
{
	call_comm("global-set-command", ed, &popup_attach, 0, NULL, "PopupTile");

	popup_map = key_alloc();

	key_add(popup_map, "Close", &popup_close);
	key_add(popup_map, "Notify:Close", &popup_notify_close);
	key_add(popup_map, "Abort", &popup_abort);
	key_add(popup_map, "popup:style", &popup_style);
	key_add(popup_map, "Refresh:size", &popup_refresh_size);
	key_add(popup_map, "popup:get-target", &popup_get_target);
	key_add(popup_map, "popup:close", &popup_do_close);
}
