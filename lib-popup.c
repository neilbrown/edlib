/*
 * Copyright Neil Brown ©2015-2020 <neil@brown.name>
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
 * A prefix to be displayed can be added by setting "prefix" on
 * the popup pane.
 * A default value can be given with attr "default" which is displated
 * after prefix
 * The event sent when the popup is closed can be set by setting
 * attribute "done-key"
 * otherwise "PopupDone" is used.
 *
 * The "Style" of a popup is a string of characters:
 * D - parent is whole display (window) rather than single pane
 * P - position w.r.t another popup - Currently always 'under'
 * M - multiple lines of text - default is one line
 * 1 - 1/4 width of parent
 * 2 - 1/2 width of parent (default)
 * 3 - 3/4 width of parent
 * 4 - full with
 * T - at top of parent (default is centred)
 * B - at bottom of parent
 * L - at left of parent (default is centred)
 * R - at right of parent
 * s - border at bottom to show document status
 * a - allow recursive popups
 * r - permit this popup even inside non-recursive popups
 * t - temporary - auto-close when focus leaves
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
	struct pane	*target safe;
	struct pane	*parent_popup;
	char		*style safe;
	struct command	*done;
};

static int line_height(struct pane *p safe, int scale)
{
	struct call_return cr =
		call_ret(all, "Draw:text-size", p, -1, NULL, "x",
			 scale, NULL, "");
	return cr.y;
}

static void popup_resize(struct pane *p safe, const char *style safe)
{
	struct popup_info *ppi = p->data;
	int x,y,w,h;
	int lh, bh = 0, bw = 0;
	char *bhs, *bws;
	struct xy xyscale = pane_scale(p);

	/* First find the size */
	lh = line_height(p, xyscale.x);
	bhs = pane_attr_get(pane_leaf(p), "border-height");
	if (bhs)
		bh = atoi(bhs);
	if (bh <= 0)
		bh = line_height(p, 0); /* border height */
	bws = pane_attr_get(pane_leaf(p), "border-width");
	if (bws)
		bw = atoi(bhs);
	if (bw <= 0)
		bw = bh;
	if (strchr(style, 'M')) {
		h = p->parent->h/2 + bh;
		attr_set_str(&p->attrs, "render-one-line", "no");
	} else {
		h = bh + lh + bh;
		attr_set_str(&p->attrs, "render-one-line", "yes");
	}
	if (ppi->parent_popup) {
		w = ppi->parent_popup->w;
		h = ppi->parent_popup->h;
		x = ppi->parent_popup->x;
		y = ppi->parent_popup->y + ppi->parent_popup->h;
	} else {
		w = p->parent->w - 2*bw;
		if (strchr(style, '1'))
			w = w / 4;
		else if (strchr(style, '3'))
			w = 3 * w / 4;
		else if (strchr(style, '4'))
			w = w;
		else
			w = w / 2;

		x = p->parent->w/2 - w/2 - bw;
		y = p->parent->h/2 - h/2 - bh;
		if (strchr(style, 'T')) { y = 0; h -= bh; }
		if (strchr(style, 'B')) { h -= bh; y = p->parent->h - h; }
		if (strchr(style, 'L')) x = 0;
		if (strchr(style, 'R')) x = p->parent->w - w;
	}
	pane_resize(p, x, y, w, h);
}

DEF_CMD(popup_close)
{
	struct popup_info *ppi = ci->home->data;

	if (ci->num)
		/* Pane had focus, so give to target */
		pane_focus(ppi->target);
	command_put(ppi->done);
	ppi->done = NULL;
	return 1;
}

DEF_CMD(popup_free)
{
	struct popup_info *ppi = ci->home->data;

	free(ppi->style);
	unalloc(ppi, pane);
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

static void popup_finished(struct pane *focus safe, struct pane *home safe,
			   const char *result)
{
	struct popup_info *ppi = home->data;
	struct pane *target = ppi->target;
	const char *key;
	struct command *done = ppi->done;

	pane_focus(target);
	key = pane_attr_get(focus, "done-key");
	if (!key)
		key = "PopupDone";

	ppi->done = NULL;
	pane_close(home);
	/* home is now closed, so ppi cannot be touched */
	if (done)
		comm_call(done, key, target, 1, NULL, result);
	else
		call(key, target, 1, NULL, result);
}

DEF_CMD(popup_abort)
{
	/* A NULL 'result' signals the aboort */
	popup_finished(ci->focus, ci->home, NULL);
	return 1;
}

DEF_CMD(popup_child_closed)
{
	/* When the child is closed, we have to disappear too, but
	 * not if there are remaining children.
	 */
	struct pane *c;

	list_for_each_entry(c, &ci->home->children, siblings)
		if (!(c->damaged & DAMAGED_CLOSED) && c->z == 0 &&
		    c != ci->focus)
			/* Still have a child */
			return 1;
	pane_close(ci->home);
	return 1;
}

static bool popup_set_style(struct pane *p safe)
{
	struct popup_info *ppi = p->data;
	char *orig_border = attr_find(p->attrs, "borders");
	bool changed = False;

	if (ppi->parent_popup) {
		char *border = pane_attr_get(ppi->parent_popup, "borders");
		attr_set_str(&p->attrs, "borders", border);
	} else {
		char border[6];
		int i, j;

		for (i = 0, j = 0; i < 4; i++) {
			if (strchr(ppi->style, "TLBR"[i]) == NULL)
				border[j++] = "TLBR"[i];
		}
		if (strchr(ppi->style, 's'))
			/* Force a status line */
			border[j++] = 's';
		border[j] = 0;
		if (!orig_border || strcmp(orig_border, border) != 0) {
			attr_set_str(&p->attrs, "borders", border);
			changed = True;
		}
	}

	if (strchr(ppi->style, 'a'))
		/* allow recursion */
		attr_set_str(&p->attrs, "Popup", "ignore");
	else
		attr_set_str(&p->attrs, "Popup", "true");
	return changed;
}

DEF_CMD(popup_style)
{
	struct popup_info *ppi = ci->home->data;

	if (!ci->str)
		return Enoarg;

	free(ppi->style);
	ppi->style = strdup(ci->str);
	if (popup_set_style(ci->home))
		call("view:changed", ci->focus);
	popup_resize(ci->home, ppi->style);
	return 1;
}

DEF_CMD(popup_notify_refresh_size)
{
	pane_damaged(ci->home, DAMAGED_SIZE);
	return 1;
}

DEF_CMD(popup_refresh_size)
{
	struct popup_info *ppi = ci->home->data;
	char *prompt, *dflt, *prefix;
	struct pane *focus = pane_leaf(ci->home);

	prefix = pane_attr_get(focus, "prefix");
	prompt = pane_attr_get(focus, "prompt");
	if (!prefix && prompt) {
		char *t = NULL;
		dflt = pane_attr_get(focus, "default");
		if (!prompt)
			prompt = "";
		if (dflt)
			asprintf(&t, "%s(%s): ", prompt, dflt);
		else
			asprintf(&t, "%s: ", prompt);
		attr_set_str(&focus->attrs, "prefix", t);
		free(t);
	}

	popup_set_style(ci->home);
	popup_resize(ci->home, ppi->style);
	return 0;
}

DEF_CMD(popup_get_target)
{
	struct popup_info *ppi = ci->home->data;
	return comm_call(ci->comm2, "callback:get-target", ppi->target);
}

DEF_CMD(popup_ignore)
{
	return 1;
}

DEF_CMD(popup_close_others)
{
	/* For some popups, like search or find-file, it doesn't make sense
	 * to maximize the popup.  For others line email-compose it does.
	 * For now, allow it on multi-line popups.
	 */
	struct popup_info *ppi = ci->home->data;
	struct pane *p;

	if (strchr(ppi->style, 'M') == NULL)
		return 1;
	p = call_ret(pane, "OtherPane", ci->focus);
	if (p) {
		home_call(ci->home->focus, "doc:attach-view", p);
		pane_focus(p);
	}
	return 1;
}

DEF_CMD(popup_split)
{
	/* Rather than 'split', this moves the popup to an 'other' pane.
	 * For some popups, like search or find-file, it doesn't make sense
	 * to allow this.  For others line email-compose it does.
	 * For now, allow it on multi-line popups.
	 */
	struct popup_info *ppi = ci->home->data;
	struct pane *p;

	if (strchr(ppi->style, 'M') == NULL)
		return 1;
	p = call_ret(pane, "OtherPane", ci->focus);
	if (p)
		p = call_ret(pane, "OtherPane", p);
	if (p) {
		home_call(ci->home->focus, "doc:attach-view", p);
		pane_focus(p);
	}
	return 1;
}

DEF_CMD(popup_set_callback)
{
	struct popup_info *ppi = ci->home->data;

	if (ppi->done)
		command_put(ppi->done);
	ppi->done = NULL;
	if (ci->comm2)
		ppi->done = command_get(ci->comm2);
	return 1;
}

DEF_CMD(popup_delayed_close)
{
	/* nothing should be using this pane any more */
	pane_close(ci->focus);
	return 1;
}

DEF_CMD(popup_defocus)
{
	struct popup_info *ppi = ci->home->data;

	if (strchr(ppi->style, 't') == NULL) {
		/* Not interested, target might be though */
		home_call(ppi->target, "pane:defocus", ci->focus);
		return Efallthrough;
	}

	if (pane_has_focus(ci->home))
		/* We are still on the focal-path from display
		 * Maybe we focussed in to a sub-popup
		 */
		return Efallthrough;
	if (call_ret(pane, "ThisPopup", ci->focus))
		/* New focus is a popup, so stay for now */
		return Efallthrough;

	call_comm("editor-on-idle", ci->home, &popup_delayed_close);

	return Efallthrough;
}

DEF_CMD(popup_this)
{
	struct popup_info *ppi = ci->home->data;

	if (strchr(ppi->style, 'a') == NULL &&
	    strcmp(ci->key, "ThisPopup") != 0)
		return Efallthrough;
	return comm_call(ci->comm2, "callback:pane", ci->home,
			 0, NULL, "Popup");
}

DEF_CMD(popup_other)
{
	/* If a popup is asked for 'Other', return the 'This'
	 * of the target
	 */
	struct popup_info *ppi = ci->home->data;

	return home_call(ppi->target, "ThisPane", ci->focus,
			 ci->num, ci->mark, ci->str,
			 ci->num2, ci->mark2, ci->str2,
			 ci->x, ci->y,
			 ci->comm2);
}

DEF_CMD(popup_child_registered)
{
	/* Anything that reponds to ThisPane needs to discard
	 * any children when new are registered.
	 */
	struct pane *p = ci->home;
	struct pane *c = ci->focus;
	struct pane *old;

	if (c->z != 0)
		return 1;
restart:
	list_for_each_entry(old, &p->children, siblings)
		if (c->z == 0 && old != c) {
			pane_close(old);
			goto restart;
		}
	p->focus = c;
	return 1;
}

static int get_scale(struct pane *p)
{
	char *sc = pane_attr_get(p, "scale");
	int scale;

	if (!sc)
		return 1000;

	scale = atoi(sc);
	if (scale > 3)
		return scale;
	return 1000;
}

DEF_CMD(popup_scale_relative)
{
	struct pane *p = ci->home;
	int scale = get_scale(p);
	int rpt = RPT_NUM(ci);

	if (rpt > 10) rpt = 10;
	if (rpt < -10) rpt = -10;
	while (rpt > 0) {
		scale = scale * 11/10;
		rpt -= 1;
	}
	while (rpt < 0) {
		scale = scale * 9 / 10;
		rpt += 1;
	}

	attr_set_int(&p->attrs, "scale", scale);
	call("view:changed", ci->focus);
	return 1;
}

DEF_CMD(popup_do_close)
{
	const char *str;

	str = ci->str;
	if (!str || !str[0])
		str = pane_attr_get(ci->focus, "default");
	if (!str)
		str = "";
	popup_finished(ci->focus, ci->home, str);
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
	 * t  - temp pane, disappears when it loses focus
	 */
	struct pane *root, *p;
	struct popup_info *ppi;
	const char *style = ci->str;
	char *in_popup;
	int z = 1;

	if (!style)
		style = "D3";

	if (!strchr(style, 'r') && !strchr(style, 'P') &&
	    (in_popup = pane_attr_get(ci->focus, "Popup")) != NULL &&
	    strcmp(in_popup, "ignore") != 0)
		/* No recusive popups without permission */
		return Efallthrough;

	if (strchr(style, 'D'))
		root = call_ret(pane, "RootPane", ci->focus);
	else if (strchr(style, 'P'))
		root = call_ret(pane, "ThisPopup", ci->focus);
	else
		root = call_ret(pane, "ThisPane", ci->focus);

	if (!root)
		return Efallthrough;

	alloc(ppi, pane);
	ppi->done = NULL;
	ppi->target = ci->focus;

	/* If focus is already a popup, make this popup higher */
	p = pane_my_child(root, ci->focus);
	if (p && p->z > 0)
		z = p->z + 1;

	ppi->parent_popup = NULL;
	if (strchr(style, 'P')) {
		ppi->parent_popup = root;
		root = root->parent;
	}

	p = pane_register(root, z + 1, &popup_handle.c, ppi);
	if (!p)
		return Efail;
	ppi->style = strdup(style);
	popup_set_style(p);
	popup_resize(p, style);
	attr_set_str(&p->attrs, "render-wrap", "no");

	pane_add_notify(p, ppi->target, "Notify:Close");
	if (ppi->parent_popup)
		pane_add_notify(p, ppi->parent_popup, "Notify:resize");

	pane_focus(p);

	if (ci->str2) {
		struct pane *doc =
			call_ret(pane, "doc:from-text", p, 0, NULL,
				 "*popup*", 0, NULL, ci->str2);
		if (doc &&
		    (p = home_call_ret(pane, doc, "doc:attach-view",
				       p, -1)) != NULL) {
			call("doc:file", p, 1);
			call("doc:set:autoclose", p, 1);
		}
	}

	if (!p)
		return Efail;

	return comm_call(ci->comm2, "callback:attach", p);
}

void edlib_init(struct pane *ed safe)
{
	call_comm("global-set-command", ed, &popup_attach,
		  0, NULL, "PopupTile");

	popup_map = key_alloc();

	key_add(popup_map, "Close", &popup_close);
	key_add(popup_map, "Free", &popup_free);
	key_add(popup_map, "Notify:Close", &popup_notify_close);
	key_add(popup_map, "Abort", &popup_abort);
	key_add(popup_map, "popup:style", &popup_style);
	key_add(popup_map, "Refresh:size", &popup_refresh_size);
	key_add(popup_map, "view:changed", &popup_refresh_size);
	key_add(popup_map, "Notify:resize", &popup_notify_refresh_size);
	key_add(popup_map, "popup:get-target", &popup_get_target);
	key_add(popup_map, "popup:close", &popup_do_close);
	key_add(popup_map, "popup:set-callback", &popup_set_callback);
	key_add(popup_map, "ChildClosed", &popup_child_closed);
	key_add(popup_map, "ThisPane", &popup_this);
	key_add(popup_map, "OtherPane", &popup_other);
	key_add(popup_map, "ThisPopup", &popup_this);
	key_add(popup_map, "ChildRegistered", &popup_child_registered);

	key_add(popup_map, "Window:bury", &popup_do_close);
	key_add(popup_map, "Window:close", &popup_abort);
	key_add(popup_map, "Window:split-x", &popup_split);
	key_add(popup_map, "Window:split-y", &popup_split);
	key_add(popup_map, "Window:x+", &popup_ignore);
	key_add(popup_map, "Window:x-", &popup_ignore);
	key_add(popup_map, "Window:y+", &popup_ignore);
	key_add(popup_map, "Window:y-", &popup_ignore);
	key_add(popup_map, "Window:close-others", &popup_close_others);
	key_add(popup_map, "Window:scale-relative", &popup_scale_relative);
	key_add(popup_map, "pane:defocus", &popup_defocus);
}
