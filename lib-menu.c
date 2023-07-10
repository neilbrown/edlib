/*
 * Copyright Neil Brown ©2022-2023 <neil@brown.name>
 * May be distributed under terms of GPLv2 - see file:COPYING
 *
 * lib-menu: support drop-down and pop-up menus.
 *
 * A menu is created by called attach-menu with x,y being location
 * in either the pane or (if str contains 'D') the dispay.
 * Entries are added by calling "menu-add" with str being the value to
 * be reported and optionally str2 being the name to display.
 *
 * A popup will be created which takes the focus. up/down moves the selection
 * and enter selects, as can the mouse.
 *
 * The selection is sent to the original focus with a callback specified in
 * str2 when the menu is attached.
 *
 */

#include "core.h"
#include "misc.h"

DEF_CMD(menu_add)
{
	struct mark *m;

	if (!ci->str)
		return Enoarg;
	m = vmark_new(ci->focus, MARK_UNGROUPED, NULL);
	call("doc:set-ref", ci->focus, 0, m);
	call("doc:list-add", ci->focus, 0, m);
	call("doc:set-attr", ci->focus, 0, m, "name", 0, NULL,
	     ci->str2 ?: ci->str);
	call("doc:set-attr", ci->focus, 0, m, "value", 0, NULL,
	     ci->str);

	mark_free(m);
	return 1;
}

DEF_CMD(menu_reposition)
{
	int lines = ci->num;
	int cols = ci->num2;
	struct pane *p = call_ret(pane, "ThisPopup", ci->focus);

	if (p && lines != 0 && cols != 0 &&
	    (lines <= p->h && cols <= p->w))
		pane_resize(p, p->x, p->y, cols, lines);
	return Efallthrough;
}

DEF_CMD(menu_abort)
{
	call("Abort", ci->focus);
	return 1;
}

DEF_CMD(menu_done)
{
	struct mark *m = ci->mark;
	const char *val;

	if (!m)
		m = call_ret(mark, "doc:point", ci->focus);
	if (!m)
		return Enoarg;
	val = pane_mark_attr(ci->focus, m, "value");
	call("popup:close", ci->focus, 0, m, val);
	return 1;
}

static struct map *menu_map;
DEF_LOOKUP_CMD(menu_handle, menu_map);

DEF_CMD(menu_attach)
{
	struct pane *docp, *p, *p2;
	/* Multi-line temporary popup with x,y location provided. */
	const char *mode = "Mtx";
	const char *mmode = ci->str ?: "";

	if (strchr(mmode, 'D'))
		/* per-display, not per-pane */
		mode = "DMtx";

	docp = call_ret(pane, "attach-doc-list", ci->focus);
	if (!docp)
		return Efail;
	call("doc:set:autoclose", docp, 1);
	attr_set_str(&docp->attrs, "render-simple", "format");
	attr_set_str(&docp->attrs, "heading", "");
	attr_set_str(&docp->attrs, "line-format", "<action-activate:menu-select>%name</>");
	attr_set_str(&docp->attrs, "done-key", ci->str2 ?: "menu-done");
	/* No borders, just a shaded background to make menu stand out */
	attr_set_str(&docp->attrs, "borders", "");
	attr_set_str(&docp->attrs, "background", "color:white-80");
	p = call_ret(pane, "PopupTile", ci->focus, 0, NULL, mode,
		     0, NULL, NULL, ci->x, ci->y);
	if (!p)
		return Efail;
	p2 = home_call_ret(pane, docp, "doc:attach-view", p,
			   0, NULL, "simple");
	if (!p2) {
		pane_close(p);
		return Efail;
	}
	p2 = pane_register(p2, 0, &menu_handle.c);
	if (!p2)
		return Efail;
	call("Mouse-grab", p2);
	return comm_call(ci->comm2, "cb:attach", p2);
}

static void menu_init_map(void)
{
	menu_map = key_alloc();

	key_add(menu_map, "render:reposition", &menu_reposition);

	key_add(menu_map, "menu-add", &menu_add);
	key_add(menu_map, "K:ESC", &menu_abort);
	key_add(menu_map, "K:Enter", &menu_done);
	key_add(menu_map, "menu-select", &menu_done);
}

void edlib_init(struct pane *ed safe)
{
	menu_init_map();
	call_comm("global-set-command", ed, &menu_attach,
		  0, NULL, "attach-menu");
}
