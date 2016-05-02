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
	if (par) {
		/* reasonable defaults */
		p->w = par->w;
		p->h = par->h;
	}
	p->abs_z = p->abs_zhi = 0;
	p->focus = NULL;
	p->handle = NULL;
	p->data = NULL;
	p->damaged = 0;
	p->pointer = NULL;
	p->attrs = NULL;
	if (par)
		pane_damaged(p, DAMAGED_SIZE);
}

static void __pane_check(struct pane *p)
{
	struct pane *c;
	list_for_each_entry(c, &p->children, siblings) {
		ASSERT(c->parent == p);
		__pane_check(c);
	}
}

static void pane_check(struct pane *p)
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
	if (!p || (p->damaged | type) == p->damaged)
		return;
	p->damaged |= type;

	p = p->parent;
	if (type & DAMAGED_SIZE)
		type = DAMAGED_SIZE_CHILD;
	else if (type & DAMAGED_NEED_CALL)
		type = DAMAGED_CHILD;
	else
		return;

	while (p && (p->damaged | type) != p->damaged) {
		p->damaged |= type;
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
	comm_call_pane(parent, "ChildRegistered", p, 0, NULL, NULL, 0, NULL);
	return p;
}

/* 'abs_z' is a global z-depth number.
 * 'abs_z' of root is 0, and abs_z of every other pane is 1 more than abs_zhi
 * of siblings with lower 'z', or same as parent if no such siblings.
 *
 * If DAMAGED_SIZE is set on a pane, we call "Refresh:size".
 * If it or DAMAGED_SIZE_CHILD was set, we recurse onto all children.
 * If abs_z is not one more than parent, we also recurse.
 */
static void pane_do_resize(struct pane *p, int damage)
{
	struct pane *c;
	int nextz;
	int abs_z = p->abs_z + 1;

	if (p->damaged & DAMAGED_CLOSED) {
		p->abs_zhi = abs_z;
		return;
	}
	damage |= p->damaged & (DAMAGED_SIZE | DAMAGED_SIZE_CHILD);
	if (!damage &&
	    (p->parent == NULL || p->abs_z == p->parent->abs_z + p->z))
		return;

	if (p->focus == NULL)
		p->focus = list_first_entry_or_null(
			&p->children, struct pane, siblings);

	if (damage & (DAMAGED_SIZE)) {
		if (comm_call(p->handle, "Refresh:size", p, 0, NULL, NULL, damage) == 0)
			pane_check_size(p);
	}

	nextz = 0;
	while (nextz >= 0) {
		int z = nextz;
		int abs_zhi = abs_z;
		nextz = -1;
		list_for_each_entry(c, &p->children, siblings) {
			if (c->z > z && (nextz == -1 || c->z < nextz))
				nextz = c->z;
			if (c->z == z) {
				if (c->abs_z != abs_z)
					c->abs_z = abs_z;
				pane_do_resize(c, damage & DAMAGED_SIZE);
				if (c->abs_zhi > abs_zhi)
					abs_zhi = c->abs_zhi;
			}
		}
		p->abs_zhi = abs_zhi;
		abs_z = abs_zhi + 1;
	}
	if (p->damaged & DAMAGED_SIZE) {
		p->damaged &= ~(DAMAGED_SIZE | DAMAGED_SIZE_CHILD);
		p->damaged |= DAMAGED_CONTENT | DAMAGED_CHILD;
	} else {
		p->damaged &= ~DAMAGED_SIZE_CHILD;
		p->damaged |= DAMAGED_CHILD;
	}
}

static void pane_do_refresh(struct pane *p, int damage, struct mark *pointer)
{
	struct pane *c;

	if (p->damaged & DAMAGED_CLOSED)
		return;

	if (p->pointer)
		pointer = p->pointer;

	damage |= p->damaged & (DAMAGED_CHILD|DAMAGED_CONTENT|DAMAGED_CURSOR);
	p->damaged = 0;
	if (!damage)
		return;
	if (list_empty(&p->children)) {
		if (damage & (DAMAGED_NEED_CALL)) {
			if (damage & DAMAGED_CONTENT)
				damage |= DAMAGED_CURSOR;
			call5("Refresh", p, 0, pointer, NULL, damage);
		}
	} else
		list_for_each_entry(c, &p->children, siblings)
			pane_do_refresh(c, damage, pointer);

	if (p->damaged & DAMAGED_POSTORDER) {
		/* post-order call was triggered */
		p->damaged &= ~DAMAGED_POSTORDER;
		comm_call(p->handle, "Refresh:postorder", p, 0, pointer, NULL, damage);
	}
}

void pane_refresh(struct pane *p)
{
	p->abs_z = 0;
	pane_do_resize(p, 0);
	pane_do_refresh(p, 0, NULL);
}

void pane_add_notify(struct pane *target, struct pane *source, char *msg)
{
	struct notifier *n = malloc(sizeof(*n));

	n->notifiee = target;
	n->notification = strdup(msg);
	n->noted = 1;
	list_add(&n->notifier_link, &source->notifiees);
	list_add(&n->notifiee_link, &target->notifiers);
}

void pane_drop_notifiers(struct pane *p, char *notification)
{
	struct list_head *t;
	struct notifier *n;

	list_for_each_entry_safe(n, t, &p->notifiers, notifiee_link) {

		if (notification && strcmp(notification, n->notification) != 0)
			continue;
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

int pane_notify(struct pane *p, char *notification, struct mark *m, struct mark *m2,
		char *str, int numeric)
{
	/* Return the largest absolute return value. If no notifiees are found.
	 * return 0
	 */
	int ret = 0;
	struct notifier *n;

	list_for_each_entry(n, &p->notifiees, notifier_link)
		n->noted = 0;
restart:
	list_for_each_entry(n, &p->notifiees, notifier_link) {
		if (n->noted)
			continue;
		n->noted = 1;
		if (strcmp(n->notification, notification) == 0) {
			int r = comm_call_pane(n->notifiee, n->notification, p,
					       numeric, m, str, 0, m2);
			if (abs(r) > abs(ret))
				ret = r;
			goto restart;
		}
	}
	return ret;
}

void pane_close(struct pane *p)
{
	struct pane *c;
	if (p->damaged & DAMAGED_CLOSED)
		return;
	p->damaged |= DAMAGED_CLOSED;
	pane_check(p);

	if (p->parent && p->parent->handle &&
	    !(p->parent->damaged & DAMAGED_CLOSED)) {
		struct cmd_info ci = {};

		ci.key = "ChildClosed";
		ci.focus = p;
		ci.home = p->parent;
		ci.comm = p->parent->handle;
		ci.comm->func(&ci);
	}
	list_del_init(&p->siblings);
	pane_drop_notifiers(p, NULL);

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
		struct cmd_info ci = {};

		ci.key = "Close";
		ci.focus = ci.home = p;
		ci.comm = p->handle;
		p->handle->func(&ci);
	}
	pane_damaged(p->parent, DAMAGED_CONTENT);
	attr_free(&p->attrs);
	free(p);
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
	newparent->damaged |= p->damaged;
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
		parent->damaged |= c->damaged;
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

	parent->damaged |= p->damaged;
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
	if (!p)
		return;
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

struct pane *render_attach(char *name, struct pane *parent)
{
	char buf[100];

	//WARN(!list_empty(&parent->children));
	if (!name)
		name = pane_attr_get(parent, "render-default");
	if (!name)
		return NULL;

	sprintf(buf, "attach-render-%s", name);
	return call_pane(buf, parent, 0, NULL, 0);
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

void pane_clear(struct pane *p, char *attrs)
{
	struct cmd_info ci = {};

	ci.key = "pane-clear";
	ci.focus = p;
	ci.str2 = attrs;
	key_handle(&ci);
}

char *pane_attr_get(struct pane *p, char *key)
{
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

void pane_clone_children(struct pane *from, struct pane *to)
{
	/* "to" is a clone of "from", but has no children.
	 * Clone all the children of "from" to "to"
	 * Ignore z>0 children
	 */
	struct pane *c;

	if (!from || !to)
		return;
	list_for_each_entry(c, &from->children, siblings) {
		if (c->z > 0)
			continue;
		comm_call_pane(c, "Clone", to, 0, NULL, NULL, 0, NULL);
	}
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
	struct cmd_info ci = {};
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
			struct mark *m, int extra, char *str, char *str2)
{
	struct cmd_info ci = {};
	struct call_return cr;

	ci.key = key;
	ci.focus = focus;
	ci.numeric = numeric;
	ci.extra = extra;
	ci.mark = m;
	ci.str = str;
	ci.str2 = str2;
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
