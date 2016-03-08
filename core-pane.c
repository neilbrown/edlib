/*
 * Copyright Neil Brown ©2015 <neil@brown.name>
 * May be distributed under terms of GPLv2 - see file:COPYING
 *
 * panes for edlib.
 *
 * There is a list of 'panes' which can display rendered content
 * and can optionally receive input.
 * A pane is registered as a child of an existing pane and indicates
 * a 'z' depth, and whether it can take input.
 *
 * The owner of a pane can:
 * - register sub-panes
 * - ask for text to be rendered at any time,
 * - can request or discard focus.  When discarded it returns to lower z level.
 *
 * The pane can tell the owner:
 * - to refresh - possibly because it has been resized
 * - that keyboard input has arrived
 * - that a mouse click has arrived
 *
 * A pane can extend beyond the size of its parent, but is always
 * clipped to the parent.  If two children of a parent overlap and
 * have the same Z-depth the result is undefined.
 */

#define _GNU_SOURCE /*  for asprintf */
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <ctype.h>
#include <stdio.h>

#include "core.h"

void pane_init(struct pane *p, struct pane *par, struct list_head *here)
{
	p->parent = par;
	if (par && !here)
		here = &par->children;
	if (here)
		list_add(&p->siblings, here);
	else
		INIT_LIST_HEAD(&p->siblings);
	INIT_LIST_HEAD(&p->children);
	INIT_LIST_HEAD(&p->notifiers);
	INIT_LIST_HEAD(&p->notifiees);
	p->x = p->y = p->z = 0;
	p->cx = p->cy = -1;
	p->h = p->w = 0;
	p->abs_z = p->abs_zhi = 0;
	p->focus = NULL;
	p->handle = NULL;
	p->data = NULL;
	p->damaged = 0;
	p->pointer = NULL;
	p->attrs = 0;
}

static void __pane_check(struct pane *p)
{
	struct pane *c;
	list_for_each_entry(c, &p->children, siblings) {
		ASSERT(c->parent == p);
		__pane_check(c);
	}
}

void pane_check(struct pane *p)
{
	while (p->parent)
		p = p->parent;
	__pane_check(p);
}
/*
 * pane_damaged: mark a pane as being 'damaged', and make
 * sure all parents know about it.
 */
void pane_damaged(struct pane *p, int type)
{
	while (p) {
		if ((p->damaged | type) == p->damaged)
			return;
		p->damaged |= type;
		type = DAMAGED_CHILD;
		p = p->parent;
	}
}

struct pane *pane_register(struct pane *parent, int z,
			   struct command *handle, void *data, struct list_head *here)
{
	struct pane *p = malloc(sizeof(*p));
	pane_init(p, parent, here);
	p->z = z;
	p->handle = handle;
	p->data = data;
	if (parent && parent->focus == NULL)
		parent->focus = p;
	return p;
}

/* 'abs_z' is a global z-depth number.
 * 'abs_z' of root is 0, and abs_z of every other pane is 1 more than abs_zhi
 * of siblings with lower 'z', or same as parent if no such siblings.
 */
static void __pane_refresh(struct cmd_info ci)
{
	struct pane *c;
	int damage = ci.extra;
	struct pane *p = ci.home;
	int ret = 0;
	int nextz;
	int abs_z = p->abs_z + 1;

	if (p->damaged & DAMAGED_CLOSED) {
		p->abs_zhi = abs_z;
		return;
	}

	if (p->focus == NULL)
		p->focus = list_first_entry_or_null(
			&p->children, struct pane, siblings);
	if (p->pointer)
		ci.mark = p->pointer;

	damage |= p->damaged;
	if (!damage)
		return;
	if (damage & (DAMAGED_NEED_CALL)) {
		struct cmd_info ci2 = ci;
		ci2.extra = damage;
		if (ci2.extra & DAMAGED_SIZE)
			ci2.extra |= DAMAGED_CONTENT;
		if (ci2.extra & DAMAGED_CONTENT)
			ci2.extra |= DAMAGED_CURSOR;
		damage &= DAMAGED_SIZE | DAMAGED_EVENTS | DAMAGED_CURSOR;
		ci2.comm = p->handle;
		ret = p->handle->func(&ci2);
		if (ret == 0)
			pane_check_size(p);
	} else
		damage = 0;
	p->damaged = 0;
	ci.extra = damage;
	nextz = 0;
	while (nextz >= 0) {
		int z = nextz;
		int abs_zhi = abs_z;
		nextz = -1;
		list_for_each_entry(c, &p->children, siblings) {
			if (c->z > z && (nextz == -1 || c->z < nextz))
				nextz = c->z;
			if (c->z == z) {
				if (c->abs_z != abs_z) {
					c->abs_z = abs_z;
					c->damaged |= DAMAGED_Z;
				}
				ci.home = c;
				__pane_refresh(ci);
				if (c->abs_zhi > abs_zhi)
					abs_zhi = c->abs_zhi;
			}
		}
		p->abs_zhi = abs_zhi;
		abs_z = abs_zhi + 1;
	}
	if (ret == 2) {
		/* "Refresh" requested a post-order call */
		ci.home = p;
		ci.numeric = 1;
		ci.comm = p->handle;
		p->handle->func(&ci);
	}
}

void pane_refresh(struct pane *p)
{
	struct cmd_info ci = {0};
	pane_damaged(p, DAMAGED_CURSOR);
	/* Always refresh a whole display */
	while (p->parent)
		p = p->parent;
	p->abs_z = 0;
	ci.focus = ci.home = p;
	ci.key = "Refresh";
	__pane_refresh(ci);
}

void pane_add_notify(struct pane *target, struct pane *source, char *msg)
{
	struct notifier *n = malloc(sizeof(*n));

	n->notifiee = target;
	n->notification = strdup(msg);
	list_add(&n->notifier_link, &source->notifiees);
	list_add(&n->notifiee_link, &target->notifiers);
}

static void pane_drop_notifiers(struct pane *p)
{
	while (!list_empty(&p->notifiers)) {
		struct notifier *n = list_first_entry(&p->notifiers,
						      struct notifier,
						      notifiee_link);
		list_del_init(&n->notifiee_link);
		list_del_init(&n->notifier_link);
		free(n->notification);
		free(n);
	}
}

void pane_notify_close(struct pane *p)
{
	while (!list_empty(&p->notifiees)) {
		struct notifier *n = list_first_entry(&p->notifiees,
						      struct notifier,
						      notifier_link);
		list_del_init(&n->notifiee_link);
		list_del_init(&n->notifier_link);
		if (strcmp(n->notification, "Notify:Close") == 0)
			comm_call_pane(n->notifiee, n->notification, p,
				       0, NULL, NULL, 0, NULL);
		free(n->notification);
		free(n);
	}
}

void pane_notify(struct pane *p, char *notification, struct mark *m, struct mark *m2)
{
	struct notifier *n;

	list_for_each_entry(n, &p->notifiees, notifier_link)
		if (strcmp(n->notification, notification) == 0)
			comm_call_pane(n->notifiee, n->notification, p,
				       0, m, NULL, 0, m2);
}

void pane_close(struct pane *p)
{
	struct pane *c;
	if (p->damaged & DAMAGED_CLOSED)
		return;
	p->damaged |= DAMAGED_CLOSED;
	pane_check(p);

	if (p->parent && p->parent->handle) {
		struct cmd_info ci = {0};

		ci.key = "ChildClosed";
		ci.focus = p;
		ci.home = p->parent;
		ci.comm = p->parent->handle;
		ci.comm->func(&ci);
	}
	list_del_init(&p->siblings);
	pane_drop_notifiers(p);

	while (!list_empty(&p->children)) {
		c = list_first_entry(&p->children, struct pane, siblings);
		pane_close(c);
	}
	if (p->parent && p->parent->focus == p) {
		pane_damaged(p->parent, DAMAGED_CURSOR);
		p->parent->focus = NULL;
	}
	pane_notify_close(p);
	if (p->handle) {
		struct cmd_info ci = {0};

		ci.key = "Close";
		ci.focus = ci.home = p;
		ci.comm = p->handle;
		p->handle->func(&ci);
	}
	pane_damaged(p->parent, DAMAGED_SIZE);
	attr_free(&p->attrs);
	free(p);
}

int pane_clone(struct pane *from, struct pane *parent)
{
	/* Create a clone of 'from' as a child of 'parent'.
	 * We send a 'Clone' message to 'from' which may
	 * well call pane_clone() recursively on children.
	 * 'parent' is passed as the 'focus'.
	 */
	struct cmd_info ci = {0};

	if (!from || !parent)
		return 0;
	ci.key = "Clone";
	ci.focus = parent;
	ci.home = from;
	ci.comm = from->handle;
	if (from->handle)
		return from->handle->func(&ci);
	return 0;
}

void pane_resize(struct pane *p, int x, int y, int w, int h)
{
	int damage = 0;
	if (x >= 0 &&
	    (p->x != x || p->y != y)) {
		damage |= DAMAGED_CONTENT;
		p->x = x;
		p->y = y;
	}
	if (w > 0 &&
	    (p->w != w || p->h != h)) {
		damage |= DAMAGED_SIZE;
		p->w = w;
		p->h = h;
	}
	pane_damaged(p, damage);
}

void pane_check_size(struct pane *p)
{
	/* match pane to parent */
	if (p->parent)
		pane_resize(p, 0, 0, p->parent->w, p->parent->h);
}

void pane_reparent(struct pane *p, struct pane *newparent)
{
	/* detach p from its parent and attach beneath its sibling newparent */
	ASSERT(p->parent == newparent->parent);
	list_del(&p->siblings);
	if (p->parent->focus == p)
		p->parent->focus = newparent;
	p->parent = newparent;
	if (newparent->focus == NULL)
		newparent->focus = p;
	list_add(&p->siblings, &newparent->children);
}

void pane_subsume(struct pane *p, struct pane *parent)
{
	/* move all content from p into parent, which must be empty,
	 * except possibly for 'p'.
	 * 'data', 'point' and 'handle' are swapped.
	 * After this, p can be freed
	 */
	void *data;
	struct command *handle;
	struct mark *point;
	struct pane *c;

	list_del_init(&p->siblings);
	if (p->parent && p->parent->focus == p) {
		pane_damaged(p->parent, DAMAGED_CURSOR);
		p->parent->focus = NULL;
	}
	p->parent = NULL;
	while (!list_empty(&p->children)) {
		c = list_first_entry(&p->children, struct pane, siblings);
		list_move(&c->siblings, &parent->children);
		c->parent = parent;
	}
	parent->focus = p->focus;

	handle = parent->handle;
	parent->handle = p->handle;
	p->handle = handle;

	data = parent->data;
	parent->data = p->data;
	p->data = data;

	point = parent->pointer;
	parent->pointer = p->pointer;
	p->pointer = point;
}

int pane_masked(struct pane *p, int x, int y, int z, int *w, int *h)
{
	/* Test if this pane, or its children, mask this location.
	 * i.e. they have a higher 'z' and might draw here.
	 * If 'w' and 'h' are set then reduce them to confirm that
	 * everything from x to x+w and y to y+h is not masked.
	 * This allows cases where there is no masking to be handled
	 * efficiently.
	 */
	struct pane *c;
	/* calculate the upper bounds */
	int xh = x + (w ? *w : 1);
	int yh = y + (h ? *h : 1);

	if (x >= p->x+p->w || y >= p->y+p->h)
		/* x,y is beyond this pane, no overlap possible */
		return 0;
	if (xh <= p->x || yh <= p->y)
		/* area is before this pane, no over lap possible */
		return 0;

	if (p->z > z) {
		/* This pane does mask some of the region */
		if (x >= p->x || y >= p->y)
			/* pane masks x,y itself */
			return 1;
		/* pane must just mask some of the region beyond x,y */
		if (w && *w > p->x - x)
			*w = p->x - x;
		if (h && *h > p->y - y)
			*h = p->y - y;

		return 0;
	}
	/* This pane doesn't mask (same z level) but a child still could */
	x -= p->x;
	y -= p->y;
	z -= p->z;
	list_for_each_entry(c, &p->children, siblings)
		if (pane_masked(c, x, y, z, w, h))
			return 1;

	return 0;
}

struct pane *pane_to_root(struct pane *p, int *x, int *y, int *z, int *w, int *h)
{
	/* The root we aim for is the display, which is two steps
	 * below the actual root (which can contain several displays
	 * each with input handler).
	 */
	while(1) {
		if (w && *x + *w > p->w)
			*w = p->w - *x;
		if (h && *y + *h > p->h)
			*h = p->h - *y;
		*x += p->x;
		*y += p->y;
		*z += p->z;
		if (!p->parent || !p->parent->parent || !p->parent->parent->parent)
			return p;
		p = p->parent;
	}
}

void pane_focus(struct pane *p)
{
	pane_damaged(p, DAMAGED_CURSOR);
	/* refocus up to the display, but not do the root */
	while (p->parent && p->parent->parent) {
		if (p->parent->focus &&
		    p->parent->focus != p) {
			pane_damaged(p->parent->focus, DAMAGED_CURSOR);
			p->parent->focus = p;
		}
		p = p->parent;
	}
}

struct editor *pane2ed(struct pane *p)
{
	while (p->parent)
		p = p->parent;
	return container_of(p, struct editor, root);
}

DEF_CMD(pane_callback)
{
	struct call_return *cr = container_of(ci->comm, struct call_return, c);
	cr->p = ci->focus;
	return 1;
}

struct pane *render_attach(char *name, struct pane *parent)
{
	char buf[100];
	struct cmd_info ci = {0};
	int ret;
	struct call_return cr;

	cr.c = pane_callback;
	cr.p = NULL;

	/* always attach a renderer as a leaf */
	parent = pane_final_child(parent);
	if (!name)
		name = pane_attr_get(parent, "default-renderer");
	if (!name)
		return NULL;

	sprintf(buf, "render-%s-attach", name);
	ci.key = buf;
	ci.focus = parent;
	ci.comm2 = &cr.c;
	ret = key_lookup(pane2ed(parent)->commands, &ci);
	if (ret)
		return cr.p;
	sprintf(buf, "render-%s", name);
	editor_load_module(pane2ed(parent), buf);
	sprintf(buf, "render-%s-attach", name);
	ret = key_lookup(pane2ed(parent)->commands, &ci);
	if (ret)
		return cr.p;
	return NULL;
}


void pane_set_mode(struct pane *p, char *mode)
{
	call5("Mode:set-mode", p, 0, NULL, mode, 0);
}

void pane_set_numeric(struct pane *p, int numeric)
{
	call3("Mode:set-numeric", p, numeric, NULL);
}

void pane_set_extra(struct pane *p, int extra)
{
	call5("Mode:set-extra", p, 0, NULL, NULL, extra);
}

struct pane *pane_attach(struct pane *p, char *type, struct pane *dp,
			 char *arg)
{
	struct cmd_info ci = {0};
	struct editor *ed = pane2ed(p);
	char *com;
	struct call_return cr;

	cr.c = pane_callback;
	cr.p = NULL;

	asprintf(&com, "attach-%s", type);
	ci.key = com;
	ci.home = dp;
	ci.focus = p;
	ci.str = arg;
	ci.comm2 = &cr.c;
	if (!key_lookup(ed->commands, &ci)) {
		char *mod;
		if (strcmp(type, "global-keymap")==0)
			type = "keymap";
		asprintf(&mod, "lib-%s", type);
		editor_load_module(ed, mod);
		free(mod);
		key_lookup(ed->commands, &ci);
	}
	free(com);
	return cr.p;
}

void pane_clear(struct pane *p, char *attrs)
{
	struct cmd_info ci = {0};

	ci.key = "pane-clear";
	ci.focus = p;
	ci.str2 = attrs;
	key_handle(&ci);
}

char *pane_attr_get(struct pane *p, char *key)
{
	struct pane *c;
	while ((c = pane_child(p)) != NULL)
		p = c;
	while (p) {
		char *a = attr_get_str(p->attrs, key, -1);
		if (a)
			return a;
		a = doc_attr(p, NULL, 0, key);
		if (a)
			return a;
		p = p->parent;
	}
	/* FIXME do I want editor-wide attributes too? */
	return NULL;
}

char *pane_mark_attr(struct pane *p, struct mark *m, int forward, char *key)
{
	while (p) {
		char *a = doc_attr(p, m, forward, key);
		if (a)
			return a;
		p = p->parent;
	}
	return NULL;
}

struct pane *pane_final_child(struct pane *p)
{
	struct pane *c;

	while ((c = pane_child(p)) != NULL)
		p = c;
	return p;
}

DEF_CMD(take_pane)
{
	struct call_return *cr = container_of(ci->comm, struct call_return, c);
	cr->p = ci->focus;
	return 1;
}

struct pane *call_pane(char *key, struct pane *focus, int numeric,
		       struct mark *m, int extra)
{
	struct cmd_info ci = {0};
	struct call_return cr;

	ci.key = key;
	ci.focus = focus;
	ci.numeric = numeric;
	ci.extra = extra;
	ci.mark = m;
	cr.c = take_pane;
	cr.p = NULL;
	ci.comm2 = &cr.c;
	if (!key_handle(&ci))
		return NULL;
	return cr.p;
}

struct pane *call_pane7(char *key, struct pane *focus, int numeric,
			struct mark *m, int extra, char *str)
{
	struct cmd_info ci = {0};
	struct call_return cr;

	ci.key = key;
	ci.focus = focus;
	ci.numeric = numeric;
	ci.extra = extra;
	ci.mark = m;
	ci.str = str;
	cr.c = take_pane;
	cr.p = NULL;
	ci.comm2 = &cr.c;
	if (!key_handle(&ci))
		return NULL;
	return cr.p;
}

/* convert pane-relative co-ords to absolute */
void pane_absxy(struct pane *p, int *x, int *y)
{
	while (p) {
		*x += p->x;
		*y += p->y;
		p = p->parent;
	}
}

/* Convert absolute c-ords to relative */
void pane_relxy(struct pane *p, int *x, int *y)
{
	while (p) {
		*x -= p->x;
		*y -= p->y;
		p = p->parent;
	}
}

void pane_map_xy(struct pane *orig, struct pane *target, int *x, int *y)
{
	pane_absxy(orig, x, y);
	pane_relxy(target, x, y);
}
