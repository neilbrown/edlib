/*
 * Copyright Neil Brown ©2015-2020 <neil@brown.name>
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
#include <time.h>
#include <string.h>
#include <wchar.h>
#include <ctype.h>
#include <stdio.h>

#include "core.h"

MEMPOOL(pane);

static void pane_init(struct pane *p safe, struct pane *par)
{
	if (par) {
		p->parent = par;
		list_add(&p->siblings, &par->children);
	} else {
		p->parent = p;
		INIT_LIST_HEAD(&p->siblings);
	}
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
	p->data = safe_cast NULL;
	p->damaged = 0;
	p->attrs = NULL;
	if (par)
		pane_damaged(p, DAMAGED_SIZE);
}

static void __pane_check(struct pane *p safe)
{
	struct pane *c;
	list_for_each_entry(c, &p->children, siblings) {
		ASSERT(c->parent == p);
		__pane_check(c);
	}
}

static void pane_check(struct pane *p safe)
{
	__pane_check(pane_root(p));
}
/*
 * pane_damaged: mark a pane as being 'damaged', and make
 * sure all parents know about it.
 */
void pane_damaged(struct pane *p, int type)
{
	int z;
	if (!p || (p->damaged | type) == p->damaged)
		return;
	if (type & (type-1)) {
		/* multiple bits are set, handle
		 * them separately
		 */
		int b;
		for (b = 1; type; b <<= 1) {
			if (b & type)
				pane_damaged(p, b);
			type &= ~b;
		}
		return;
	}
	p->damaged |= type;
	if (type == DAMAGED_SIZE)
		pane_notify("Notify:resize", p);

	z = p->z;
	if (z < 0)
		/* light-weight pane - never propagate damage */
		return;
	p = p->parent;
	if (type == DAMAGED_SIZE)
		type = DAMAGED_SIZE_CHILD;
	else if (type == DAMAGED_VIEW)
		type = DAMAGED_VIEW_CHILD;
	else if (type & DAMAGED_NEED_CALL)
		type = DAMAGED_CHILD;
	else if (type == DAMAGED_POSTORDER)
		type = DAMAGED_POSTORDER_CHILD;
	else
		return;

	while ((p->damaged | type) != p->damaged) {
		if (z > 0 && (type & DAMAGED_SIZE_CHILD))
			/* overlay changed size, so we must refresh */
			p->damaged |= DAMAGED_CONTENT;
		p->damaged |= type;
		z = p->z;
		p = p->parent;
	}
}

struct pane *__pane_register(struct pane *parent, short z,
			     struct command *handle safe,
			     void *data, short data_size)
{
	struct pane *p;

	alloc(p, pane);
	pane_init(p, parent);
	p->z = z;
	p->handle = command_get(handle);
	if (!data)
		/* type of 'data' should correlate with type of handle,
		 * which should be parameterised...
		 */
		p->data = handle;
	else
		p->data = data;
	p->data_size = data_size;
	if (z >= 0) {
		if (parent && parent->focus == NULL)
			parent->focus = p;
		pane_call(parent, "ChildRegistered", p);
		if (p->damaged & DAMAGED_CLOSED)
			/* ChildRegistered objected */
			p = NULL;
	}
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
static void pane_do_resize(struct pane *p safe, int damage)
{
	struct pane *c;
	int nextz;
	int abs_z = p->abs_z + 1;

	if (p->damaged & DAMAGED_CLOSED) {
		p->abs_zhi = abs_z;
		return;
	}
	if ((damage & DAMAGED_SIZE) && p->z == 0)
		/* Parent was resized and didn't propagate, so we need to */
		pane_resize(p, 0, 0, p->parent->w, p->parent->h);

	damage |= p->damaged & (DAMAGED_SIZE | DAMAGED_SIZE_CHILD);
	if (!damage &&
	    p->abs_z == p->parent->abs_z + abs(p->z))
		return;

	if (damage & (DAMAGED_SIZE))
		if (pane_call(p, "Refresh:size", p, 0, NULL,
			      NULL, damage) != 0)
			/* No need to propagate, just check on children */
			damage = 0;

	nextz = 0;
	while (nextz >= 0) {
		int z = nextz;
		int abs_zhi = abs_z;
		nextz = -1;
		list_for_each_entry(c, &p->children, siblings)
			c->damaged |= DAMAGED_NOT_HANDLED;
	restart:
		list_for_each_entry(c, &p->children, siblings) {
			if (c->damaged & DAMAGED_NOT_HANDLED)
				c->damaged &= ~DAMAGED_NOT_HANDLED;
			else
				/* Only handle each pane once */
				continue;
			if (c->z < 0) {
				c->abs_z = c->parent->abs_z;
				continue;
			}
			if (c->z > z && (nextz == -1 || c->z < nextz))
				nextz = c->z;
			if (c->z == z) {
				if (c->abs_z != abs_z)
					c->abs_z = abs_z;
				pane_do_resize(c, damage & DAMAGED_SIZE);
				if (c->abs_zhi > abs_zhi)
					abs_zhi = c->abs_zhi;
				/* Pane could have been disconnected, must restart */
				goto restart;
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

static void pane_do_refresh(struct pane *p safe, int damage)
{
	struct pane *c;
	int sent = 0;

	if (p->damaged & DAMAGED_CLOSED)
		return;

	damage |= p->damaged & (DAMAGED_CHILD|DAMAGED_CONTENT|DAMAGED_CURSOR);
	p->damaged &= ~(DAMAGED_CHILD|DAMAGED_CONTENT|DAMAGED_CURSOR);
	if (!damage)
		return;
	list_for_each_entry(c, &p->children, siblings)
		c->damaged |= DAMAGED_NOT_HANDLED;
restart:
	list_for_each_entry(c, &p->children, siblings) {
		if (c->damaged & DAMAGED_NOT_HANDLED)
			c->damaged &= ~DAMAGED_NOT_HANDLED;
		else
			/* Only handle each pane once */
			continue;
		if (c->z >= 0) {
			sent = 1;
			pane_do_refresh(c, damage);
			goto restart;
		}
	}
	if (!sent && damage & (DAMAGED_NEED_CALL)) {
		if (damage & DAMAGED_CONTENT)
			damage |= DAMAGED_CURSOR;
		call("Refresh", p, 0, NULL, NULL, damage);
	}
}

static void pane_do_review(struct pane *p safe, int damage)
{
	struct pane *c;
	int sent = 0;

	if (p->damaged & DAMAGED_CLOSED)
		return;

	damage |= p->damaged & (DAMAGED_VIEW|DAMAGED_VIEW_CHILD);
	p->damaged &= ~(DAMAGED_VIEW|DAMAGED_VIEW_CHILD);
	if (!damage)
		return;
	list_for_each_entry(c, &p->children, siblings)
		c->damaged |= DAMAGED_NOT_HANDLED;
restart:
	list_for_each_entry(c, &p->children, siblings) {
		if (c->damaged & DAMAGED_NOT_HANDLED)
			c->damaged &= ~DAMAGED_NOT_HANDLED;
		else
			/* Only handle each pane once */
			continue;
		if (c->z >= 0) {
			sent = 1;
			pane_do_review(c, damage);
			goto restart;
		}
	}
	if (!sent && damage & (DAMAGED_VIEW))
		call("Refresh:view", p, 0, NULL, NULL, damage);
}

static void pane_do_postorder(struct pane *p safe)
{
	struct pane *c;
	int damage;

	if (p->damaged & DAMAGED_CLOSED)
		return;

	damage = p->damaged & (DAMAGED_POSTORDER|DAMAGED_POSTORDER_CHILD);
	p->damaged &= ~(DAMAGED_POSTORDER|DAMAGED_POSTORDER_CHILD);
	if (!damage)
		return;

	list_for_each_entry(c, &p->children, siblings)
		c->damaged |= DAMAGED_NOT_HANDLED;
restart:
	list_for_each_entry(c, &p->children, siblings) {
		if (c->damaged & DAMAGED_NOT_HANDLED)
			c->damaged &= ~DAMAGED_NOT_HANDLED;
		else
			/* Only handle each pane once */
			continue;
		pane_do_postorder(c);
		goto restart;
	}
	if (damage & DAMAGED_POSTORDER)
		call("Refresh:postorder", p);
}

void pane_refresh(struct pane *p safe)
{
	int cnt = 5;
	if (p->parent == p)
		p->abs_z = 0;

	while (cnt-- && (p->damaged & ~DAMAGED_CLOSED)) {
		pane_do_resize(p, 0);
		pane_do_review(p, 0);
		pane_do_refresh(p, 0);
		pane_do_postorder(p);
	}
	if (p->damaged) {
		static time_t last_warn;
		static int rpt;
		if (last_warn + 5 < time(NULL))
			rpt = 0;
		if (rpt++ < 5)
			LOG("WARNING %sroot pane damaged after refresh: %d",
			    p->parent != p ? "":"non-", p->damaged);
		last_warn = time(NULL);
		call("editor:notify:Message:broadcast",p, 0, NULL,
		     "Refresh looping - see log");
	}
}

void pane_add_notify(struct pane *target safe, struct pane *source safe,
		     const char *msg safe)
{
	struct notifier *n;

	list_for_each_entry(n, &source->notifiees, notifier_link)
		if (n->notifiee == target &&
		    strcmp(msg, n->notification) == 0)
			/* Already notifying */
			return;

	alloc(n, pane);

	n->notifiee = target;
	n->notification = strdup(msg);
	n->noted = 1;
	list_add(&n->notifier_link, &source->notifiees);
	list_add(&n->notifiee_link, &target->notifiers);
}

void pane_drop_notifiers(struct pane *p safe, char *notification)
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

static void pane_notify_close(struct pane *p safe)
{
	while (!list_empty(&p->notifiees)) {
		struct notifier *n = list_first_entry(&p->notifiees,
						      struct notifier,
						      notifier_link);
		list_del_init(&n->notifiee_link);
		list_del_init(&n->notifier_link);
		if (strcmp(n->notification, "Notify:Close") == 0)
			pane_call(n->notifiee, n->notification, p);
		free(n->notification);
		free(n);
	}
}

int do_pane_notify(struct pane *home, const char *notification safe,
		   struct pane *p safe,
		   int num, struct mark *m, const char *str,
		   int num2, struct mark *m2, const char *str2,
		   struct command *comm2)
{
	/* Return the largest absolute return value. If no notifiees are found.
	 * return 0
	 */
	int ret = 0;
	int cnt = 0;
	struct notifier *n, *n2;

	if (!home)
		home = p;
	/* FIXME why no error below */
	list_for_each_entry_reverse(n, &home->notifiees, notifier_link)
		if (strcmp(n->notification, notification) == 0) {
			if (n->noted == 2)
				/* Nested notification - fail */
				return Efail;
			n->noted = 0;
		}
restart:
	list_for_each_entry(n, &home->notifiees, notifier_link) {
		if (n->noted)
			continue;
		if (strcmp(n->notification, notification) == 0) {
			int r;
			n->noted = 2;
			r = pane_call(n->notifiee, notification, p,
				      num, m, str,
				      num2, m2, str2, cnt,ret, comm2);
			if (abs(r) > abs(ret))
				ret = r;
			cnt += 1;
			/* Panes might have been closed or notifications removed
			 * so nothing can be trusted... except this home pane
			 * had better still exist.
			 */
			list_for_each_entry(n2, &home->notifiees, notifier_link)
				if (n2 == n && n->noted == 2) {
					/* Still safe .. */
					n->noted = 1;
					continue;
				}
			/* 'n' has been removed, restart */
			goto restart;
		}
	}
	return ret;
}

static void pane_refocus(struct pane *p safe)
{
	struct pane *c;

	pane_damaged(p, DAMAGED_CURSOR);
	p->focus = NULL;
	/* choose the worst credible focus - the oldest.
	 * Really some else should be updating the focus, this is
	 * just a fall-back
	 */
	list_for_each_entry_reverse(c, &p->children, siblings)
		if (c->z >= 0) {
			p->focus = c;
			break;
		}
}

void pane_close(struct pane *p safe)
{
	struct pane *c;
	struct pane *ed;

	if (p->damaged & DAMAGED_CLOSED)
		return;
	p->damaged |= DAMAGED_CLOSED;
	pane_check(p);

	ed = pane_root(p);

	pane_drop_notifiers(p, NULL);

	if (!(p->parent->damaged & DAMAGED_CLOSED))
		pane_call(p->parent, "ChildClosed", p);
	list_del_init(&p->siblings);

restart:
	list_for_each_entry(c, &p->children, siblings) {
		if (c->damaged & DAMAGED_CLOSED)
			continue;
		pane_close(c);
		goto restart;
	}

	if (p->parent->focus == p)
		pane_refocus(p->parent);

	pane_notify_close(p);
	pane_call(p, "Close", p);

	if (p->z >= 0)
		pane_damaged(p->parent, DAMAGED_CONTENT);
	/* If a child has not yet had "Close" called, we need to leave
	 * ->parent in place so a full range of commands are available.
	 */
	if (ed != p) {
		p->damaged |= DAMAGED_DEAD;
		editor_delayed_free(ed, p);
	} else {
		pane_call(p, "Free", p);
		command_put(p->handle);
		p->handle = NULL;
		attr_free(&p->attrs);
		free(p);
	}
}

void pane_resize(struct pane *p safe, int x, int y, int w, int h)
{
	int damage = 0;

	if (x >= 0 &&
	    (p->x != x || p->y != y)) {
		damage |= DAMAGED_CONTENT | DAMAGED_SIZE;
		p->x = x;
		p->y = y;
	}
	if (w > 0 &&
	    (p->w != w || p->h != h)) {
		damage |= DAMAGED_SIZE;
		p->w = w;
		p->h = h;
	}
	if (p->w < 0 || p->h < 0)
		/* FIXME something more gentle */
		abort();
	if (p->w <= 0)
		p->w = 1;
	if (p->h <= 0)
		p->h = 1;
	pane_damaged(p, damage);
}

void pane_reparent(struct pane *p safe, struct pane *newparent safe)
{
	/* detach p from its parent and attach beneath its sibling newparent */
	int replaced = 0;
	// FIXME this should be a failure, possibly with warning, not an
	// assert.  I'm not sure just now how best to do warnings.
	ASSERT(newparent->parent == p->parent);
	list_del(&p->siblings);
	if (p->parent->focus == p)
		p->parent->focus = newparent;
	if (newparent->parent == newparent) {
		newparent->parent = p->parent;
		list_add(&newparent->siblings, &p->parent->children);
		pane_resize(newparent, 0, 0, p->parent->w, p->parent->h);
		replaced = 1;
	}
	p->parent = newparent;
	newparent->damaged |= p->damaged;
	if (newparent->focus == NULL)
		newparent->focus = p;
	list_add(&p->siblings, &newparent->children);
	pane_call(newparent->parent, "ChildMoved", p);
	if (replaced)
		pane_call(newparent->parent, "ChildReplaced", newparent);
}

void pane_move_after(struct pane *p safe, struct pane *after)
{
	/* Move 'p' after 'after', or if !after, move to start */
	if (p == p->parent || p == after)
		return;
	if (after) {
		if (p->parent != after->parent)
			return;
		list_move(&p->siblings, &after->siblings);
	} else {
		list_move(&p->siblings, &p->parent->children);
	}
}

void pane_subsume(struct pane *p safe, struct pane *parent safe)
{
	/* move all content from p into parent, which must be empty,
	 * except possibly for 'p'.
	 * 'data' and 'handle' are swapped.
	 * Finally, p is freed
	 */
	void *data;
	struct command *handle;
	struct pane *c;

	list_del_init(&p->siblings);
	if (p->parent->focus == p)
		pane_refocus(p->parent);

	p->parent = pane_root(parent);
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

	parent->damaged |= p->damaged;

	pane_close(p);
}

int pane_masked(struct pane *p safe, short x, short y, short abs_z,
		short *w, short *h)
{
	/* Test if this pane, or its children, mask this location.
	 * i.e. they have a higher 'abs_z' and might draw here.
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

	if (p->abs_z > abs_z && p->z > 0) {
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
	list_for_each_entry(c, &p->children, siblings)
		if (pane_masked(c, x, y, abs_z, w, h))
			return 1;

	return 0;
}

void pane_focus(struct pane *focus)
{
	struct pane *p = focus;
	if (!p)
		return;
	pane_damaged(p, DAMAGED_CURSOR);
	/* refocus up to the display, but not to the root */
	/* We have root->input->display. FIXME I need a better way
	 * to detect the 'display' level.
	 */
	for (; p->parent->parent->parent != p->parent->parent; p = p->parent) {
		struct pane *old = p->parent->focus;
		if (old == p)
			continue;
		p->parent->focus = p;
		if (old) {
			pane_damaged(old, DAMAGED_CURSOR);
			while (old->focus)
				old = old->focus;
			call("pane:defocus", old);
		}
	}
	call("pane:refocus", focus);
}

char *pane_attr_get(struct pane *p, const char *key safe)
{
	while (p) {
		char *a = attr_find(p->attrs, key);

		if (a)
			return a;
		a = CALL(strsave, pane, p, "get-attr", p, 0, NULL, key);
		if (a)
			return a;
		if (p == p->parent)
			return NULL;
		p = p->parent;
	}
	return NULL;
}

char *pane_mark_attr(struct pane *p safe, struct mark *m safe,
		     const char *key safe)
{
	return call_ret(strsave, "doc:get-attr", p, 0, m, key);
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
	list_for_each_entry(c, &from->children, siblings)
		c->damaged |= DAMAGED_NOT_HANDLED;
restart:
	list_for_each_entry(c, &from->children, siblings) {
		if (c->damaged & DAMAGED_NOT_HANDLED)
			c->damaged &= ~DAMAGED_NOT_HANDLED;
		else
			/* Only handle each pane once */
			continue;
		if (c->z > 0)
			continue;
		pane_call(c, "Clone", to);
		goto restart;
	}
}

struct pane *pane_my_child(struct pane *p, struct pane *c)
{
	while (c && c->parent != p) {
		if (c->parent == c)
			return NULL;
		c = c->parent;
	}
	return c;
}

DEF_CMD(take_simple)
{
	struct call_return *cr = container_of(ci->comm, struct call_return, c);
	cr->p = ci->focus;
	cr->m = ci->mark;
	cr->m2 = ci->mark2;
	cr->i = ci->num;
	cr->i2 = ci->num2;
	cr->x = ci->x;
	cr->y = ci->y;
	cr->comm = ci->comm2;
	cr->s = strsave(ci->focus, ci->str);
	return 1;
}

DEF_CMD(take_str)
{
	struct call_return *cr = container_of(ci->comm, struct call_return, c);

	if (!ci->str)
		return 0;
	cr->s = strdup(ci->str);
	return 1;
}


struct pane *do_call_pane(enum target_type type, struct pane *home,
			  struct command *comm2a,
			  const char *key safe, struct pane *focus safe,
			  int num,  struct mark *m,  const char *str,
			  int num2, struct mark *m2, const char *str2,
			  int x, int y, struct command *comm2b,
			  struct commcache *ccache)
{
	struct call_return cr = {};

	cr.c = take_simple;
	cr.ret = do_call_val(type, home, comm2a, key, focus, num, m, str,
			     num2, m2, str2, x, y, &cr.c, ccache);
	if (cr.ret < 0)
		return NULL;
	return cr.p;
}

struct mark *do_call_mark(enum target_type type, struct pane *home,
			  struct command *comm2a,
			  const char *key safe, struct pane *focus safe,
			  int num,  struct mark *m,  const char *str,
			  int num2, struct mark *m2, const char *str2,
			  int x, int y, struct command *comm2b,
			  struct commcache *ccache)
{
	struct call_return cr = {};

	cr.c = take_simple;
	cr.ret = do_call_val(type, home, comm2a, key, focus, num, m, str,
			     num2, m2, str2, x, y, &cr.c, ccache);
	if (cr.ret < 0)
		return NULL;
	return cr.m;
}

struct mark *do_call_mark2(enum target_type type, struct pane *home,
			   struct command *comm2a,
			   const char *key safe, struct pane *focus safe,
			   int num,  struct mark *m,  const char *str,
			   int num2, struct mark *m2, const char *str2,
			   int x, int y, struct command *comm2b,
			   struct commcache *ccache)
{
	struct call_return cr = {};

	cr.c = take_simple;
	cr.ret = do_call_val(type, home, comm2a, key, focus, num, m, str,
			     num2, m2, str2, x, y, &cr.c, ccache);
	if (cr.ret < 0)
		return NULL;
	return cr.m2;
}

struct command *do_call_comm(enum target_type type, struct pane *home,
			     struct command *comm2a,
			     const char *key safe, struct pane *focus safe,
			     int num,  struct mark *m,  const char *str,
			     int num2, struct mark *m2, const char *str2,
			     int x, int y, struct command *comm2b,
			     struct commcache *ccache)
{
	struct call_return cr = {};

	cr.c = take_simple;
	cr.ret = do_call_val(type, home, comm2a, key, focus, num, m, str,
			     num2, m2, str2, x, y, &cr.c, ccache);
	if (cr.ret < 0)
		return NULL;
	return cr.comm;
}

char *do_call_strsave(enum target_type type, struct pane *home,
		      struct command *comm2a,
		      const char *key safe, struct pane *focus safe,
		      int num,  struct mark *m,  const char *str,
		      int num2, struct mark *m2, const char *str2,
		      int x, int y, struct command *comm2b,
		      struct commcache *ccache)
{
	struct call_return cr = {};

	cr.c = take_simple;
	cr.ret = do_call_val(type, home, comm2a, key, focus, num, m, str,
			     num2, m2, str2, x, y, &cr.c, ccache);
	return cr.s;
}

struct call_return do_call_all(enum target_type type, struct pane *home,
			       struct command *comm2a,
			       const char *key safe, struct pane *focus safe,
			       int num,  struct mark *m,  const char *str,
			       int num2, struct mark *m2, const char *str2,
			       int x, int y, struct command *comm2b,
			       struct commcache *ccache)
{
	struct call_return cr = {};

	cr.c = take_simple;
	cr.ret = do_call_val(type, home, comm2a, key, focus, num, m, str,
			     num2, m2, str2, x, y, &cr.c, ccache);
	return cr;
}

char *do_call_str(enum target_type type, struct pane *home,
		  struct command *comm2a,
		  const char *key safe, struct pane *focus safe,
		  int num,  struct mark *m,  const char *str,
		  int num2, struct mark *m2, const char *str2,
		  int x, int y, struct command *comm2b,
		  struct commcache *ccache)
{
	struct call_return cr = {};

	cr.c = take_str;
	cr.ret = do_call_val(type, home, comm2a, key, focus, num, m, str,
			     num2, m2, str2, x, y, &cr.c, ccache);
	if (cr.ret < 0) {
		free(cr.s);
		return NULL;
	}
	return cr.s;
}

/* convert pane-relative co-ords to absolute */
void pane_absxy(struct pane *p, short *x safe, short *y safe,
		short *w safe, short *h safe)
{
	while (p) {
		if (p->w > 0 && *x + *w > p->w)
			*w = p->w - *x;
		if (p->h > 0 && *y + *h > p->h)
			*h = p->h - *y;
		*x += p->x;
		*y += p->y;
		if (p->parent == p)
			break;
		p = p->parent;
	}
}

/* Convert absolute c-ords to relative */
void pane_relxy(struct pane *p, short *x safe, short *y safe)
{
	while (p) {
		*x -= p->x;
		*y -= p->y;
		if (p->parent == p)
			break;
		p = p->parent;
	}
}

void pane_map_xy(struct pane *orig, struct pane *target,
		 short *x safe, short *y safe)
{
	short w=0, h=0;
	if (orig != target) {
		pane_absxy(orig, x, y, &w, &h);
		pane_relxy(target, x, y);
	}
}

struct xy pane_scale(struct pane *p safe)
{
	/* "scale" is roughly pixels-per-point * 1000
	 * So 10*scale.x is the width of a typical character in default font.
	 * 10*scale.y is the height.
	 * scale.x should be passed to text-size and and Draw:text to get
	 * correctly sized text
	 *
	 */
	char *scM = pane_attr_get(p, "scale:M");
	char *sc;
	struct xy xy;
	int w,h;
	int mw, mh;
	int scale;

	if (!scM ||
	    sscanf(scM, "%dx%d", &mw, &mh) != 2 ||
	    mw <= 0 || mh <= 0) {
		/* Fonts have fixed 1x1 size so scaling not supported */
		xy.x = 100;
		xy.y = 100;
		return xy;
	}
	sc = pane_attr_get(p, "scale");
	if (sc == NULL)
		scale = 1000;
	else if (sscanf(sc, "x:%d,y:%d", &w, &h) == 2 ||
		 sscanf(sc, "%dx%d", &w, &h) == 2) {
		/* choose scale so w,h point fits in pane */
		int xscale, yscale;
		if (w <= 0) w = 1;
		if (h <= 0) h = 1;
		xscale = 1000 * p->w * 10 / mw / w;
		yscale = 1000 * p->h * 10 / mh / h;
		if (sc[0] == 'x')
			/* Old style where 'y' was in 'width' units... */
			yscale *= 2;
		scale = (xscale < yscale) ? xscale :  yscale;
	} else if (sscanf(sc, "%d", &scale) != 1)
		scale = 1000;

	if (scale < 10)
		scale = 10;
	if (scale > 100000)
		scale = 100000;
	xy.x = scale * mw / 10;
	xy.y = scale * mh / 10;
	return xy;
}
