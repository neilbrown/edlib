/*
 * Copyright Neil Brown ©2022-2023 <neil@brown.name>
 * May be distributed under terms of GPLv2 - see file:COPYING
 *
 * lib-menu: support drop-down and pop-up menus.
 *
 * A menu is created by called attach-menu with x,y being location
 * in either the pane or (if str contains 'D') the dispay.
 * Entries are added by calling "menu-add" with str being the value to
 * be displayed (the name) and optionally str2 being a different value
 * to be reported (the action).
 *
 * A popup will be created which takes the focus. up/down moves the selection
 * and enter selects, as can the mouse.
 *
 * The selection is sent to the original focus with a callback specified in
 * str2 when the menu is attached.
 *
 */

#define PANE_DATA_VOID
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
	     ci->str);
	call("doc:set-attr", ci->focus, 0, m, "action", 0, NULL,
	     ci->str2 ?: ci->str);
	if (ci->num & 1)
		call("doc:set-attr", ci->focus, 0, m, "disabled",
		     0, NULL, "1");

	mark_free(m);
	return 1;
}

DEF_CMD(menu_clear)
{
	struct mark *m = vmark_new(ci->focus, MARK_UNGROUPED, NULL);

	call("doc:set-ref", ci->home, 1, m);
	while (call("doc:list-del", ci->home, 0, m) > 0)
		;
	return 1;
}

DEF_CMD(menu_attr)
{
	if (ci->str && strcmp(ci->str, "FG") == 0) {
		char *s = call_ret(str, "doc:get-attr", ci->home,
				   0, ci->mark, "disabled");
		char *v = (s && *s) ? "fg:white-40" : "fg:black";
		comm_call(ci->comm2, "cb", ci->focus, 0, ci->mark,
			  v, 0, NULL, ci->str);
		free(s);
		return 1;
	}
	if (ci->str && strcmp(ci->str, "fg") == 0) {
		char *s = call_ret(str, "doc:get-attr", ci->home,
				   0, ci->mark, "disabled");
		char *v = (s && *s) ? "fg:blue+60" : "fg:blue";
		comm_call(ci->comm2, "cb", ci->focus, 0, ci->mark,
			  v, 0, NULL, ci->str);
		free(s);
		return 1;
	}
	if (ci->str && strcmp(ci->str, "shortcut") == 0) {
		char *s = call_ret(str, "doc:get-attr", ci->home,
				   0, ci->mark, "action");
		/* a leading space on 'action' suppresses listing as a shortcut */
		char *v = (s && *s != ' ') ? s : "";
		comm_call(ci->comm2, "cb", ci->focus, 0, ci->mark,
			  v, 0, NULL, ci->str);
		free(s);
		return 1;
	}
	return Efallthrough;
}

DEF_CMD(menu_reposition)
{
	int lines = ci->num;
	int cols = ci->num2;
	struct pane *p = call_ret(pane, "ThisPopup", ci->focus);

	if (!p || lines <= 0 || cols <= 0)
		return Efallthrough;
	if (lines > p->parent->h - p->y)
		lines = p->parent->h - p->y;
	if (cols > p->parent->w - p->x)
		cols = p->parent->w - p->x;
	/* Add 1 to cols so that if menu gets wider we will see that and resize */
	pane_resize(p, p->x, p->y, cols+1, lines);
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
	val = pane_mark_attr(ci->focus, m, "action");
	call("popup:close", ci->focus, 0, m, val);
	return 1;
}

static struct map *menu_map;
DEF_LOOKUP_CMD(menu_handle, menu_map);

DEF_CMD(menu_attach)
{
	/* ->str gives the "mode"
	 * D  means per-display menu, not per-pane
	 * V  means show value in menu as well as name
	 * F  means to use the focus as the doc, and its
	 *    parent as the focus.
	 * ->str2 gives command to call on completion, else
	 *    "menu-done" is used.
	 * ->x,y are co-ordinated relative to ->focus where menu
	 *    (Top-Left) appears
	 * ->comm2 returns the created pane.
	 */
	struct pane *docp, *p, *p2;
	/* Multi-line temporary popup with x,y location provided. */
	const char *mode = "Mtx";
	const char *mmode = ci->str ?: "";
	struct pane *focus = ci->focus;

	if (strchr(mmode, 'D'))
		/* per-display, not per-pane */
		mode = "DMtx";

	if (strchr(mmode, 'F')) {
		docp = focus;
		focus = focus->parent;
	} else {
		docp = call_ret(pane, "attach-doc-list", ci->focus);
		if (!docp)
			return Efail;
		call("doc:set:autoclose", docp, 1);
		attr_set_str(&docp->attrs, "render-simple", "format");
		attr_set_str(&docp->attrs, "heading", "");
		if (strchr(mmode, 'V'))
			/* show the 'action' - presumably a key name */
			attr_set_str(&docp->attrs, "line-format",
				     "<%FG><action-activate:menu-select>%name <rtab><%fg>%shortcut</></></>");
		else
			attr_set_str(&docp->attrs, "line-format",
				     "<%FG><action-activate:menu-select>%name</></>");
		attr_set_str(&docp->attrs, "done-key", ci->str2 ?: "menu-done");
		/* No borders, just a shaded background to make menu stand out */
		attr_set_str(&docp->attrs, "borders", "");
		attr_set_str(&docp->attrs, "background", "color:white-80");
	}
	p = call_ret(pane, "PopupTile", focus, 0, NULL, mode,
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
	/* Don't allow any shift - we size the menu to fit */
	if (!p2)
		return Efail;
	attr_set_int(&p2->attrs, "render-wrap", 0);
	call("Mouse-grab", p2);
	return comm_call(ci->comm2, "cb:attach", p2);
}

static void menu_init_map(void)
{
	menu_map = key_alloc();

	key_add(menu_map, "render:reposition", &menu_reposition);

	key_add(menu_map, "menu-add", &menu_add);
	key_add(menu_map, "menu-clear", &menu_clear);
	key_add(menu_map, "Cancel", &menu_abort);
	key_add(menu_map, "K:Enter", &menu_done);
	key_add(menu_map, "menu-select", &menu_done);
	key_add(menu_map, "doc:get-attr", &menu_attr);
}

void edlib_init(struct pane *ed safe)
{
	menu_init_map();
	call_comm("global-set-command", ed, &menu_attach,
		  0, NULL, "attach-menu");
	call_comm("global-set-command", ed, &menu_add,
		  0, NULL, "menu:add");
	call_comm("global-set-command", ed, &menu_clear,
		  0, NULL, "menu:clear");
}
