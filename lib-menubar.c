/*
 * Copyright Neil Brown ©2023 <neil@brown.name>
 * May be distributed under terms of GPLv2 - see file:COPYING
 *
 * trim a line off the top line of a pane and place a menu
 * bar.   Actions are sent to the focus.
 *
 * We place a renderline at the top and construct a string
 * to give to it as needed.
 * We create menu documents as children of the main pane,
 * and display them as needed.
 *
 * Menus are added either to LHS or RHS and must be added in order
 * So FILE EDIT VIEW must be in order, and HELP on right before
 * any other right-side menus.
 * Before displayed a menu, the pane which requested it is given
 * a chance to update the content via a "menu:refresh" notification.
 *
 * Menus are created and populated with "menubar-add" which acts
 * like menu-add,  The name is "X/Y" where is the name of the menu
 * and Y is the name in the menu.  If X doesn't exist, the menu
 * is created.  If Y already exists, the other details are updated.
 * menubar-delete and menubar-clear can delete individual menus,
 * or clear all entries so they can be repopulated.
 *
 * Menu documents are collected as children of this pane.  The focus
 * of each document is the pane which requested the window.  This
 * allows the menu to be discarded when that pane is closed, and to
 * be hidden when the pane loses focus.
 *
 * Child panes have ->z value:
 * 0 for child and bar
 * 1 or more for active menu
 * -1 for menu documents created by in-focus clients
 * -2 for menu documents created by not-in-focus clients
 */

#include <stdio.h>
#define PANE_DATA_TYPE struct mbinfo
#include "core.h"

struct mbinfo {
	struct pane *bar safe;
	struct pane *child;
	struct pane *menu, *open;
	bool hidden, wanted;
};
#include "core-pane.h"

static struct map *menubar_map;
DEF_LOOKUP_CMD(menubar_handle, menubar_map);

DEF_CMD(menubar_border)
{
	struct mbinfo *mbi = ci->home->data;

	mbi->hidden = !mbi->wanted || ci->num <= 0;
	pane_damaged(ci->home, DAMAGED_SIZE);
	return Efallthrough;
}

DEF_CMD(menubar_refresh_size)
{
	struct mbinfo *mbi = ci->home->data;
	struct pane *p = mbi->bar;

	if (mbi->hidden) {
		/* Put bar below window - out of sight */
		pane_resize(p, 0, ci->home->h,
			    p->w, p->h);
		if (mbi->child)
			pane_resize(mbi->child, 0, 0,
				    ci->home->w, ci->home->h);
	} else {
		pane_resize(p, 0, 0, ci->home->w, ci->home->h/3);
		call("render-line:measure", p, -1);
		if (mbi->child && ci->home->h > p->h)
			pane_resize(mbi->child, 0, p->h,
				    ci->home->w, ci->home->h - p->h);
	}
	pane_damaged(ci->home, DAMAGED_VIEW);
	return 1;
}

DEF_CMD(menubar_child_notify)
{
	struct mbinfo *mbi = ci->home->data;

	if (ci->focus->z)
		/* Ignore */
		return 1;
	if (ci->num < 0) {
		if (ci->home->focus == ci->focus)
			ci->home->focus = NULL;
		mbi->child = NULL;
	} else {
		if (mbi->child)
			pane_close(mbi->child);
		mbi->child = ci->focus;
		ci->home->focus = ci->focus;
	}
	return 1;
}

DEF_CMD(menubar_refresh)
{
	struct buf b;
	struct pane *home = ci->home;
	struct mbinfo *mbi = home->data;
	struct pane *p;
	struct pane *bar = mbi->bar;
	int h;

	if (mbi->hidden)
		return 1;
	if (!mbi->child)
		return 1;
	buf_init(&b);
	buf_concat(&b, ACK SOH "tab:20" STX);

	list_for_each_entry(p, &home->children, siblings) {
		char *n, *c;
		if (p->z >= 0)
			continue;
		if (!p->focus)
			/* Strange - every doc should have a focus... */
			continue;
		p->x = -1;
		p->z = -2;
		if (!pane_has_focus(p->focus, mbi->child))
			/* Owner of this menu not in focus */
			continue;
		n = pane_attr_get(p, "doc-name");
		if (!n || !*n)
			continue;
		for (c = n ; *c; c++)
			if (*c == ',' || (*c > 0 && *c < ' '))
				*c = '_';
		if (mbi->menu && mbi->open == p)
			buf_concat(&b, SOH "fg:black,bg:white-80,"
				   "menu-name:");
		else
			buf_concat(&b, SOH "fg:blue,underline,"
				   "menu-name:");
		buf_concat(&b, n);
		buf_concat(&b, STX);
		buf_concat(&b, n);
		buf_concat(&b, ETX " ");
		p->x = b.len;
		p->z = -1;
	}
	buf_concat(&b, ETX);
	h = bar->h;
	call("render-line:set", bar, -1, NULL, buf_final(&b),
	     0, NULL, "bg:#ffa500+50");
	pane_resize(bar, 0, 0, bar->w, home->h/3);
	call("render-line:measure", bar, -1);
	if (bar->h != h)
		pane_damaged(home, DAMAGED_SIZE);
	free(buf_final(&b));
	return 1;
}

enum create_where {
	C_NOWHERE,
	C_LEFT,
	C_RIGHT,
};
static struct pane *menubar_find(struct pane *home safe,
				 struct pane *owner,
				 const char *name safe,
				 enum create_where create)
{
	struct pane *p, *m, *d;
	char *a;
	struct pane *last_left = NULL;

	list_for_each_entry(p, &home->children, siblings) {
		if (p->z >= 0)
			continue;
		if (!p->focus)
			/* Strange - every doc should have a focus... */
			continue;
		/* If no owner, then we only want currently visible docs */
		if (!owner && p->z != -1)
			continue;
		a = pane_attr_get(p, "doc-name");
		if (owner && p->focus != owner)
			continue;
		if (!a || strcmp(name, a) != 0)
			continue;
		return p;
	}
	if (create == C_NOWHERE || !owner)
		return NULL;
	m = call_ret(pane, "attach-menu", home, 0, NULL, "DV", 0, NULL,
		     "menubar-done");
	if (!m)
		return NULL;
	d = call_ret(pane, "doc:get-doc", m);
	if (d)
		call("doc:set:autoclose", d, 0);
	call("popup:close", m);
	if (!d)
		return NULL;
	call("doc:set-name", d, 0, NULL, name);
	call("doc:set:menubar-side", d, 0, NULL,
	     create == C_LEFT ? "left" : "right");
	/* Find insertion point */
	list_for_each_entry(p, &home->children, siblings) {
		if (p->z >= 0)
			continue;
		if (!p->focus)
			/* Strange - every doc should have a focus... */
			continue;
		a = pane_attr_get(p, "menubar-side");
		if (a && strcmp(a, "left") == 0)
			last_left = p;
	}
	d->z = -1;
	pane_reparent(d, home);
	d->focus = owner;
	pane_add_notify(home, owner, "Notify:Close");
	if (last_left)
		list_move(&d->siblings, &last_left->siblings);
	else if (create == C_RIGHT)
		list_move_tail(&d->siblings, &home->children);
	pane_damaged(home, DAMAGED_VIEW);
	return d;
}

DEF_CMD(menubar_add)
{
	const char *val;
	char *menu;
	struct pane *d;

	if (!ci->str || !ci->str2)
		return Enoarg;

	val = strchr(ci->str, '/');
	if (!val)
		return Enoarg;
	menu = strndup(ci->str, val - ci->str);
	val += 1;
	d = menubar_find(ci->home, ci->focus, menu,
			 ci->num & 2 ? C_RIGHT : C_LEFT);
	if (!d) {
		free(menu);
		return Efail;
	}
	call("menu:add", d, 0, NULL, val, 0, NULL, ci->str2);
	return 1;
}

DEF_CMD(menubar_delete)
{
	struct pane *d;

	if (!ci->str)
		return Enoarg;
	d = menubar_find(ci->home, ci->focus, ci->str, C_NOWHERE);
	if (!d)
		return Efail;
	pane_close(d);
	return 1;
}

DEF_CMD(menubar_clear)
{
	struct pane *d;

	if (!ci->str)
		return Enoarg;
	d = menubar_find(ci->home, ci->focus, ci->str, C_NOWHERE);
	if (!d)
		return Efail;
	call("menu:clear", d);
	return 1;
}

DEF_CMD(menubar_done)
{
	struct pane *home = ci->home;
	struct mbinfo *mbi = home->data;

	if (mbi->child)
		pane_focus(mbi->child);
	call("Keystroke-sequence", home, 0, NULL, ci->str);
	return 1;
}

DEF_CMD(menubar_root)
{
	/* Provide a pane for popup to attach to */
	comm_call(ci->comm2, "cb", ci->home);
	return 1;
}

DEF_CMD(menubar_view_changed)
{
	//struct mbinfo *mbi = ci->home->data;

	//ci->home->focus = mbi->child;
	return 1;
}

DEF_CMD(menubar_press)
{
	struct mbinfo *mbi = ci->home->data;
	struct call_return cr;
	struct xy cih;
	struct pane *p;
	int x, y;

	if (ci->focus != mbi->bar)
		return Efallthrough;
	if (mbi->menu) {
		call("popup:close", mbi->menu);
		mbi->menu = NULL;
		pane_damaged(ci->home, DAMAGED_VIEW);
	}
	cih = pane_mapxy(mbi->bar, ci->home,
			 ci->x == INT_MAX ? ci->focus->cx : ci->x,
			 ci->y == INT_MAX ? ci->focus->cy : ci->y,
			 False);
	cr = pane_call_ret(all, mbi->bar, "render-line:findxy",
			   mbi->bar, -1, NULL, NULL,
			   0, NULL, NULL,
			   cih.x, cih.y);
	if (cr.ret <= 0)
		return 1;
	if (cr.s && sscanf(cr.s, "%dx%d,", &x, &y) == 2) {
		cih.x = x;
		cih.y = y;
	}
	list_for_each_entry(p, &ci->home->children, siblings) {
		if (p->z != -1)
			continue;
		if (!p->focus)
			continue;
		if (p->x < cr.ret - 1)
			continue;
		/* clicked on 'p' */
		mbi->menu = call_ret(pane, "attach-menu", p, 0, NULL, "DVF",
				     0, NULL, NULL,
				     cih.x, mbi->bar->h);
		if (mbi->menu) {
			pane_add_notify(ci->home, mbi->menu, "Notify:Close");
			mbi->open = p;
		}
		pane_damaged(ci->home, DAMAGED_VIEW);
		return 1;
	}
	return 1;
}

DEF_CMD(menubar_release)
{
	struct mbinfo *mbi = ci->home->data;
	struct pane *c = pane_my_child(ci->home, ci->focus);

	if (c == mbi->child)
		return Efallthrough;

	/* any button maps to -3 for menu action */
	return home_call(ci->home->parent, "M:Release-3", ci->focus,
			 ci->num, ci->mark, ci->str,
			 ci->num2, ci->mark2, ci->str2,
			 ci->x, ci->y, ci->comm2);
}

DEF_CMD(menubar_close_notify)
{
	struct mbinfo *mbi = ci->home->data;
	struct pane *p;

	if (ci->focus == mbi->menu) {
		mbi->menu = NULL;
		pane_damaged(ci->home, DAMAGED_VIEW);
		return 1;
	}
	if (ci->focus == mbi->child) {
		mbi->child = NULL;
		return 1;
	}
	if (ci->focus == mbi->bar) {
		// FIXME
		return 1;
	}
	list_for_each_entry(p, &ci->home->children, siblings) {
		if (p->z >= 0)
			continue;
		if (p->focus == ci->focus) {
			p->focus = NULL;
			pane_close(p);
			return 1;
		}
	}
	return 1;
}

DEF_CMD(menubar_attach)
{
	struct pane *ret, *mbp;
	struct mbinfo *mbi;
	char *v = pane_attr_get(ci->focus, "menubar-visible");

	ret = pane_register(ci->focus, 0, &menubar_handle.c);
	if (!ret)
		return Efail;
	mbi = ret->data;
	mbi->wanted = True;
	if (v && strcmp(v, "no") == 0)
		mbi->wanted = False;
	mbi->hidden = ! mbi->wanted;
	mbp = call_ret(pane, "attach-renderline", ret, 1);
	if (!mbp) {
		pane_close(ret);
		return Efail;
	}
	mbi->bar = mbp;
	pane_damaged(ret, DAMAGED_VIEW);
	return comm_call(ci->comm2, "callback:attach", ret);
}

void edlib_init(struct pane *ed safe)
{
	call_comm("global-set-command", ed, &menubar_attach,
		  0, NULL, "attach-menubar");

	menubar_map = key_alloc();
	key_add(menubar_map, "Display:border", &menubar_border);
	key_add(menubar_map, "Refresh:size", &menubar_refresh_size);
	key_add(menubar_map, "Child-Notify", &menubar_child_notify);
	key_add(menubar_map, "Refresh:view", &menubar_refresh);
	key_add(menubar_map, "menubar-add", &menubar_add);
	key_add(menubar_map, "menubar-delete", &menubar_delete);
	key_add(menubar_map, "menubar-clear", &menubar_clear);
	key_add(menubar_map, "menubar-done", &menubar_done);
	key_add(menubar_map, "RootPane", &menubar_root);
	key_add(menubar_map, "Notify:Close", &menubar_close_notify);
	key_add(menubar_map, "view:changed", &menubar_view_changed);
	key_add_prefix(menubar_map, "M:Press-", &menubar_press);
	key_add_prefix(menubar_map, "M:Release-", &menubar_release);
}
