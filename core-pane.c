/*
 * Copyright Neil Brown ©2015-2023 <neil@brown.name>
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

#define PANE_DATA_PTR_TYPE void *
#include "core.h"
#include "core-pane.h"
#include "internal.h"

MEMPOOL(pane);

static void pane_init(struct pane *p safe, struct pane *par)
{
	if (par) {
		p->parent = par;
		list_add(&p->siblings, &par->children);
		p->root = par->root;
	} else {
		p->parent = p;
		INIT_LIST_HEAD(&p->siblings);
		p->root = p;
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
	p->abs_z = 0;
	p->focus = NULL;
	p->handle = NULL;
	p->damaged = 0;
	p->attrs = NULL;
	p->refs = 1;
}

static void _pane_check(struct pane *p safe)
{
	struct pane *c;
	list_for_each_entry(c, &p->children, siblings) {
		ASSERT(c->parent == p);
		_pane_check(c);
	}
}

static void pane_check(struct pane *p safe)
{
	_pane_check(pane_root(p));
}

DEF_CMD(pane_refresh);
/*
 * pane_damaged: mark a pane as being 'damaged', and make
 * sure all parents know about it.
 */
void pane_damaged(struct pane *p, int type)
{
	int z;
	struct pane *orig = p;
	int orig_type = type;
	if (!p || (p->damaged | type) == p->damaged)
		return;
	if (p == p->parent && !p->damaged)
		if (call_comm("event:on-idle", p, &pane_refresh, 1) <= 0)
			/* Cannot register an event yet, ignore damage */
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

	z = p->z;
	if (z < 0)
		/* light-weight pane - never propagate damage */
		return;
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

	p->damaged |= type;
	p = p->parent;
	while ((p->damaged | type) != p->damaged) {
		if (p->damaged & DAMAGED_DEBUG) {
			LOG("damage %s %d (%d)", orig->name, orig_type, type);
			LOG_BT();
		}

		if (p == p->parent && !p->damaged)
			call_comm("event:on-idle", p, &pane_refresh, 1);

		if (z > 0 && (type & DAMAGED_SIZE_CHILD))
			/* overlay changed size, so we must refresh */
			/* FIXME should this be a notification? */
			p->damaged |= DAMAGED_REFRESH;
		p->damaged |= type;
		z = p->z;
		p = p->parent;
	}
}

static struct pane *_do_pane_register(struct pane *parent, short z,
				       struct command *handle safe,
				       void *data, short data_size)
{
	struct pane *p;
	short alloc_size = data_size;

	if (data)
		alloc_size = sizeof(data);
	alloc_size += offsetof(struct pane, data);

	p = alloc_zbuf(alloc_size, pane);
	pane_init(p, parent);
	p->alloc_size = alloc_size;
	p->z = z;
	if (parent)
		p->abs_z = parent->abs_z + 1;
	p->handle = command_get(handle);
	if (data)
		p->data = data;

	p->name = handle->name;
	if (z >= 0) {
		if (parent && parent->focus == NULL)
			parent->focus = p;
		if (pane_call(parent, "Child-Notify", p, 1) < 0 ||
		    p->damaged & DAMAGED_CLOSED) {
			/* ChildRegistered objected */
			p->damaged |= DAMAGED_NOINIT;
			pane_close(p);
			p = NULL;
		} else if (parent && parent != pane_root(parent))
			/* Need to set size of child, but not
			 * if parent is root and that make libevent
			 * fail to load.
			 */
			pane_damaged(parent, DAMAGED_SIZE);
	}
	return p;
}

struct pane *do_pane_register(struct pane *parent safe, short z,
			      struct command *handle safe,
			      void *data, short data_size)
{
	return _do_pane_register(parent, z, handle, data, data_size);
}

struct pane *pane_register_root(struct command *handle safe,
				void *data, short data_size)
{
	return _do_pane_register(NULL, 0, handle, data, data_size);
}

void pane_update_handle(struct pane *p safe, struct command *handle safe)
{
	command_put(p->handle);
	p->handle = command_get(handle);
}

/* 'abs_z' is a global z-depth number (->z is relative to parent)
 * 'abs_z' of root is 0, and abs_z of every other pane with z<=0 is 1 more than
 * abs_z or parent, and abs_z of pane with z>0 is 1 more than max abs_z
 * of all children of siblings with lower z.
 */

static int pane_do_absz(struct pane *p safe, int absz)
{
	int nextz = 0;
	struct pane *c;

	p->abs_z = absz;
	absz += 1;
	while (nextz >= 0) {
		int z = nextz;
		int next_absz = absz + 1;

		nextz = -1;
		list_for_each_entry(c, &p->children, siblings) {
			int t;

			if (c->z < 0 && z == 0) {
				c->abs_z = p->abs_z + 1;
				continue;
			}
			if (c->z < z)
				continue;
			if (c->z > z) {
				if (nextz < 0 || c->z < nextz)
					nextz = c->z;
				continue;
			}

			t = pane_do_absz(c, absz);
			if (t > next_absz)
				next_absz = t;
		}
		absz = next_absz;
	}
	return absz + 1;
}

/*
 * If DAMAGED_SIZE is set on a pane, we call "Refresh:size".
 * If it or DAMAGED_SIZE_CHILD was set, we recurse onto all children.
 * If abs_z is not one more than parent, we also recurse.
 */
static void pane_do_resize(struct pane *p safe, int damage)
{
	int loops = 0;
	if (!(p->damaged & DAMAGED_SIZE_CHILD))
		return;

	while (p->damaged & DAMAGED_SIZE_CHILD) {
		struct pane *parent, *c, *t;

		/* Find a child with DAMGED_SIZE_CHILD.
		 * If it is DAMAGED_SIZE, handle that, else check
		 * its children.  If no DAMAGED_SIZE_CHILD
		 * children are found, clear DAMAGED_SIZE_CHILD.
		 */
		c = p;
		while (c && !(c->damaged & DAMAGED_CLOSED)) {
			parent = c;
			c = NULL;
			if (parent->damaged & DAMAGED_SIZE) {
				parent->damaged &= ~DAMAGED_SIZE;
				loops += 1;
				if (loops == 1000) {
					LOG("pane resize looped 1000 times - some pane must keep damaging itself.");
					call("editor:notify:Message:broadcast",
					     p, 0, NULL,
					     "WARNING pane resize loop - see log");
				}
				if (loops >= 1000)
					break;

				if (pane_call(parent, "Refresh:size",
					      parent) == Efallthrough) {
					/* Need to resize children ourselves */
					list_for_each_entry(c, &parent->children,
							    siblings)
						if (c->z == 0)
							pane_resize(c, 0, 0,
								    parent->w,
								    parent->h);
						else if (c->z > 0)
							pane_damaged(c, DAMAGED_SIZE);
				}
				/* Need to restart from root */
				break;
			}
			list_for_each_entry(t, &parent->children, siblings)
				if (t->damaged & DAMAGED_SIZE_CHILD) {
					c = t;
					break;
				}
			if (!c)
				parent->damaged &= ~DAMAGED_SIZE_CHILD;
		}
	}
	pane_do_absz(p, 0);
}

static void pane_do_refresh(struct pane *p safe)
{
	struct pane *c;
	int damage;

	if (p->damaged & DAMAGED_CLOSED)
		return;

	damage = p->damaged & (DAMAGED_CHILD|DAMAGED_REFRESH);
	if (!damage)
		return;
	p->damaged &= ~damage;
	if (damage & DAMAGED_REFRESH)
		pane_call(p, "Refresh", pane_focus(p));

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
			pane_do_refresh(c);
			goto restart;
		}
	}
}

static void pane_do_review(struct pane *p safe)
{
	struct pane *c;
	int damage;

	if (p->damaged & DAMAGED_CLOSED)
		return;

	damage = p->damaged & (DAMAGED_VIEW|DAMAGED_VIEW_CHILD);
	p->damaged &= ~damage;
	if (!damage)
		return;

	if (damage & DAMAGED_VIEW)
		pane_call(p, "Refresh:view", pane_focus(p));

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
			pane_do_review(c);
			goto restart;
		}
	}
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

REDEF_CMD(pane_refresh)
{
	struct pane *p = ci->home;
	int cnt = 5;

	if (p->damaged & DAMAGED_CLOSED)
		return 1;
	call("editor:notify:Refresh-active", p, 1);
	time_start(TIME_REFRESH);
	while (cnt-- &&
	       (p->damaged &
		~(DAMAGED_CLOSED|DAMAGED_POSTORDER|DAMAGED_POSTORDER_CHILD))) {
		if (cnt == 0)
			p->damaged |= DAMAGED_DEBUG;
		pane_do_resize(p, 0);
		pane_do_review(p);
		pane_do_refresh(p);
		p->damaged &= ~DAMAGED_DEBUG;
	}
	if (cnt < 0)
		p->damaged |= DAMAGED_DEBUG;
	pane_do_postorder(p);
	p->damaged &= ~DAMAGED_DEBUG;
	if (p->damaged) {
		static time_t last_warn;
		static int rpt;
		if (last_warn + 5 < time(NULL))
			rpt = 0;
		if (rpt++ < 5)
			LOG("WARNING %sroot pane damaged after refresh: 0x%x",
			    p->parent != p ? "":"non-", p->damaged);
		last_warn = time(NULL);
		call("editor:notify:Message:broadcast",p, 0, NULL,
		     "Refresh looping - see log");
	}
	time_stop(TIME_REFRESH);
	call("editor:notify:Refresh-active", p, 0);
	return 1;
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
	n->noted = 0;
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
	list_for_each_entry_reverse(n, &home->notifiees, notifier_link)
		if (strcmp(n->notification, notification) == 0 &&
		    n->noted) {
			/* Nested notifications are not allowed. We cannot
			 * be sure the outer notification is sent, and it might
			 * contain different information.
			 */
			LOG("Nested notification from %s to %s for %s not permitted.",
			    home->name, n->notifiee->name, notification);
			LOG_BT();
			call("editor:notify:Message:broadcast",
			     home, 0, NULL,
			     "WARNING nested notification blocked - see log");
			return Efail;
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
	list_for_each_entry(n, &home->notifiees, notifier_link)
		if (strcmp(n->notification, notification) == 0)
			n->noted = 0;
	return ret;
}

static void pane_refocus(struct pane *p safe)
{
	struct pane *c;
	struct mark *pt;

	p->focus = NULL;
	/* choose the worst credible focus - the oldest.
	 * Really some one else should be updating the focus, this is
	 * just a fall-back
	 */
	list_for_each_entry_reverse(c, &p->children, siblings)
		if (c->z >= 0) {
			p->focus = c;
			break;
		}
	/* Tell the new focus to update - probably just a cursor update */
	p = pane_focus(p);
	pt = call_ret(mark, "doc:point", p);
	call("view:changed", p, 0, pt);
}

void pane_close(struct pane *p safe)
{
	struct pane *c;
	struct pane *ed;
	bool infocus;
	bool again;

	if (p->damaged & DAMAGED_CLOSED)
		return;
	p->damaged |= DAMAGED_CLOSED;
	pane_check(p);

	ed = pane_root(p);

	/* Lots of panes might need to be notified of our closing.
	 * We try to notify the more "distant" first, so children are
	 * last and those registered for notification are first.
	 * Parent comes beween.
	 */
	pane_notify_close(p);

	if (!(p->parent->damaged & DAMAGED_CLOSED))
		pane_call(p->parent, "Child-Notify", p, -1);
	list_del_init(&p->siblings);

	do {
		again = False;
		list_for_each_entry(c, &p->children, siblings) {
			if (c->damaged & DAMAGED_CLOSED)
				continue;
			pane_close(c);
			again = True;
			break;
		}
	} while (again);

	/* It is important not to drop notifiers until after all dependant
	 * panes are closed, as their closing might send a notification back
	 * to this pane.  That happens with documents when the holder of
	 * a mark-view is closed.
	 */
	pane_drop_notifiers(p, NULL);

	infocus = pane_has_focus(p);
	if (!(p->parent->damaged & DAMAGED_CLOSED) &&
	    p->parent->focus == p)
		pane_refocus(p->parent);

	if (!(p->damaged & DAMAGED_NOINIT))
		pane_call(p, "Close", p, infocus);

	/* If a child has not yet had "Close" called, we need to leave
	 * ->parent in place so a full range of commands are available.
	 */
	p->damaged |= DAMAGED_DEAD;
	if (ed != p) {
		editor_delayed_free(ed, p);
	} else {
		command_put(p->handle);
		p->handle = NULL;
		attr_free(&p->attrs);
		pane_put(p);
	}
}

void pane_free(struct pane *p safe)
{
	if (p->refs == 0)
		unalloc_buf_safe(p, p->alloc_size, pane);
}

bool pane_resize(struct pane *p safe, int x, int y, int w, int h)
{
	int damage = 0;

	if (p->x != x || p->y != y) {
		damage |= DAMAGED_SIZE;
		p->x = x;
		p->y = y;
	}
	if (w >= 0 &&
	    (p->w != w || p->h != h)) {
		damage |= DAMAGED_SIZE;
		p->w = w;
		p->h = h;
	}
	if (p->w < 0 || p->h < 0) {
		if (p->w < 0)
			p->w = 0;
		if (p->h < 0)
			p->h = 0;
	}
	/* tell the pane to resize its children later */
	pane_damaged(p, damage);
	if (damage)
		pane_notify("Notify:resize", p);
	return !!damage;
}

void pane_reparent(struct pane *p safe, struct pane *newparent safe)
{
	/* Change the parent of 'p' to be 'newparent.
	 * An important consideration is that a pane must never move up the
	 * hierarchy (towards the root), but only sideways or down.  This ensures
	 * that any mark the pane (or a descendant thereof) might hold still gets
	 * delivered to the correct document.  There is one exception that a pane
	 * that was newly created may be re-attached above some ancestors.
	 * There is not currently any test for "newness" though that may be added
	 * later (FIXME)
	 *
	 * 'newparent' must be a descendant of p->parent.
	 * If it is a sibling of p or descentant thereof, p is simply detached from
	 * its parent and reattached below newparent.
	 * If it is a descendant of p (it cannot be p itself), then as well as p
	 * being detached from it parent and attached to newparent, newparent is detached
	 * and attached between p and p->parent, thus ensuring no loop is created.
	 */
	int replaced = 0;
	struct pane *pc = pane_my_child(p->parent, newparent);
	if (pc == NULL || newparent == p) {
		LOG("Cannot reparent %s to %s, new parent must be a sibling or their descendant",
		    p->name, newparent->name);
		LOG_BT();
		return;
	}
	/* Detatch p */
	list_del_init(&p->siblings);
	if (p->parent->focus == p)
		p->parent->focus = pc;
	if (pc == p) {
		p->parent->focus = NULL;
		/* newparent is below p, need to detach and reattach it */
		if (newparent->parent->focus == newparent)
			newparent->parent->focus = NULL;
		pane_call(newparent->parent, "Child-Notify", newparent, -2);
		newparent->parent = p->parent;
		list_move(&newparent->siblings, &p->parent->children);
		pane_resize(newparent, 0, 0, p->parent->w, p->parent->h);
		replaced = 1;
	}
	pane_call(p->parent, "Child-Notify", p, -2);
	/* Reattach p under newparent */
	p->parent = newparent;
	newparent->damaged |= p->damaged;
	if (newparent->focus == NULL)
		newparent->focus = p;
	list_add(&p->siblings, &newparent->children);
	pane_call(p->parent, "Child-Notify", p, 2);
	if (replaced)
		pane_call(newparent->parent, "Child-Notify", newparent, 2);
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
	pane_damaged(p, DAMAGED_SIZE);

	pane_close(p);
}

void pane_take_focus(struct pane *focus)
{
	struct pane *p = focus;
	struct mark *pt;
	if (!p)
		return;
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
			old = pane_focus(old);
			pt = call_ret(mark, "doc:point", old);
			call("view:changed", old, 0, pt);
			home_call(old, "pane:defocus", focus);
		}
	}
	pt = call_ret(mark, "doc:point", pane_focus(focus));
	call("view:changed", pane_focus(focus), 0, pt);
	call("pane:refocus", focus);
}

bool do_pane_has_focus(struct pane *focus, struct pane *root)
{
	/* Test inf pane_take_focus() would change anything. */
	struct pane *p = focus;

	if (!p)
		return False;
	/* We check down to the declared root, or to on1 above
	 * the global root.  Where the focus of the global root
	 * is never matters.
	 */
	for (;
	     p != root
	     && p->parent->parent->parent != p->parent->parent;
	     p = p->parent)
		if (p->parent->focus != p)
			return False;
	return True;
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
	/* "to" is a clone of "from", but has no children or attributes.
	 * Copy the attributes and
	 * clone all the children of "from" to "to"
	 * Ignore z>0 children
	 */
	struct pane *c;

	if (!from || !to)
		return;
	if (from->attrs && !to->attrs)
		to->attrs = attr_copy(from->attrs);
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

struct pane * safe pane_leaf(struct pane *p safe)
{
	/* Find the only child with ->z of zero,
	 * and recurse on that.
	 * This ignores popups and stops when a pane
	 * splits.
	 */
	struct pane *l = p;

	while (l) {
		struct pane *c;
		p = l;
		l = NULL;
		list_for_each_entry(c, &p->children, siblings) {
			if (c->z)
				continue;
			if (!l) {
				l = c;
				continue;
			}
			/* Two candidates, so further leaf - stop here */
			l = NULL;
			break;
		}
	}
	return p;
}

DEF_CB(take_simple)
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

DEF_CB(take_str)
{
	struct call_return *cr = container_of(ci->comm, struct call_return, c);

	if (!ci->str)
		return Enoarg;
	cr->s = strdup(ci->str);
	return 1;
}

DEF_CB(take_comm)
{
	struct call_return *cr = container_of(ci->comm, struct call_return, c);
	if (ci->comm2)
		cr->comm = command_get(ci->comm2);
	return 1;
}

DEF_CMD(take_bytes)
{
	struct call_return *cr = container_of(ci->comm, struct call_return, c);

	if (!ci->str || ci->num < 0 || ci->num > 1000*1000*1000)
		return Enoarg;
	cr->s = malloc(ci->num);
	cr->i = ci->num;
	if (cr->s)
		memcpy(cr->s, ci->str, ci->num);
	return 1;
}

struct pane *do_call_pane(enum target_type type, struct pane *home,
			  struct command *comm2a,
			  const char *key safe, struct pane *focus safe,
			  int num,  struct mark *m,  const char *str,
			  int num2, struct mark *m2, const char *str2,
			  int x, int y, struct command *comm2b)
{
	struct call_return cr = {};

	cr.c = take_simple;
	cr.ret = do_call_val(type, home, comm2a, key, focus, num, m, str,
			     num2, m2, str2, x, y, &cr.c);
	if (cr.ret < 0)
		return NULL;
	return cr.p;
}

struct mark *do_call_mark(enum target_type type, struct pane *home,
			  struct command *comm2a,
			  const char *key safe, struct pane *focus safe,
			  int num,  struct mark *m,  const char *str,
			  int num2, struct mark *m2, const char *str2,
			  int x, int y, struct command *comm2b)
{
	struct call_return cr = {};

	cr.c = take_simple;
	cr.ret = do_call_val(type, home, comm2a, key, focus, num, m, str,
			     num2, m2, str2, x, y, &cr.c);
	if (cr.ret < 0)
		return NULL;
	return cr.m;
}

struct mark *do_call_mark2(enum target_type type, struct pane *home,
			   struct command *comm2a,
			   const char *key safe, struct pane *focus safe,
			   int num,  struct mark *m,  const char *str,
			   int num2, struct mark *m2, const char *str2,
			   int x, int y, struct command *comm2b)
{
	struct call_return cr = {};

	cr.c = take_simple;
	cr.ret = do_call_val(type, home, comm2a, key, focus, num, m, str,
			     num2, m2, str2, x, y, &cr.c);
	if (cr.ret < 0)
		return NULL;
	return cr.m2;
}

struct command *do_call_comm(enum target_type type, struct pane *home,
			     struct command *comm2a,
			     const char *key safe, struct pane *focus safe,
			     int num,  struct mark *m,  const char *str,
			     int num2, struct mark *m2, const char *str2,
			     int x, int y, struct command *comm2b)
{
	struct call_return cr = {};

	cr.c = take_comm;
	cr.ret = do_call_val(type, home, comm2a, key, focus, num, m, str,
			     num2, m2, str2, x, y, &cr.c);
	if (cr.ret < 0)
		return NULL;
	return cr.comm;
}

char *do_call_strsave(enum target_type type, struct pane *home,
		      struct command *comm2a,
		      const char *key safe, struct pane *focus safe,
		      int num,  struct mark *m,  const char *str,
		      int num2, struct mark *m2, const char *str2,
		      int x, int y, struct command *comm2b)
{
	struct call_return cr = {};

	cr.c = take_simple;
	cr.ret = do_call_val(type, home, comm2a, key, focus, num, m, str,
			     num2, m2, str2, x, y, &cr.c);
	return cr.s;
}

struct call_return do_call_all(enum target_type type, struct pane *home,
			       struct command *comm2a,
			       const char *key safe, struct pane *focus safe,
			       int num,  struct mark *m,  const char *str,
			       int num2, struct mark *m2, const char *str2,
			       int x, int y, struct command *comm2b)
{
	struct call_return cr = {};

	cr.c = take_simple;
	cr.ret = do_call_val(type, home, comm2a, key, focus, num, m, str,
			     num2, m2, str2, x, y, &cr.c);
	return cr;
}

char *do_call_str(enum target_type type, struct pane *home,
		  struct command *comm2a,
		  const char *key safe, struct pane *focus safe,
		  int num,  struct mark *m,  const char *str,
		  int num2, struct mark *m2, const char *str2,
		  int x, int y, struct command *comm2b)
{
	struct call_return cr = {};

	cr.c = take_str;
	cr.ret = do_call_val(type, home, comm2a, key, focus, num, m, str,
			     num2, m2, str2, x, y, &cr.c);
	if (cr.ret < 0) {
		free(cr.s);
		return NULL;
	}
	return cr.s;
}

struct call_return do_call_bytes(enum target_type type, struct pane *home,
				 struct command *comm2a,
				 const char *key safe, struct pane *focus safe,
				 int num,  struct mark *m,  const char *str,
				 int num2, struct mark *m2, const char *str2,
				 int x, int y, struct command *comm2b)
{
	struct call_return cr = {};

	cr.c = take_bytes;
	cr.ret = do_call_val(type, home, comm2a, key, focus, num, m, str,
			     num2, m2, str2, x, y, &cr.c);
	if (cr.ret < 0) {
		free(cr.s);
		cr.s = NULL;
	}
	return cr;
}

struct xy pane_mapxy(struct pane *p safe, struct pane *target safe,
		     short x, short y, bool clip)
{
	/* x and y are relative to p.  The result xy is relative to target */
	struct xy xy;

	/* This is a bit of a hack, but is needed to map lib-renderline
	 * co-ordes to a pane which is parallel with the render-line
	 * pane, but might be further from the root.
	 * We move 'target' towards the root to a pane of exactly the
	 * same size and position.  This will not change a correct
	 * result, and can make a correct result more likely.
	 */
	while (target->parent != target &&
	       target->x == 0 && target->y == 0 &&
	       target->parent->w == target->w &&
	       target->parent->h == target->h)
		target = target->parent;

	while (p != target && p != p->parent) {
		if (clip && p->w > 0) {
			if (x < 0)
				x = 0;
			if (x > p->w)
				x = p->w;
		}
		if (clip && p->h > 0) {
			if (y < 0)
				y = 0;
			if (y > p->h)
				y = p->h;
		}
		x += p->x;
		y += p->y;
		p = p->parent;
	}
	xy.x = x;
	xy.y = y;
	return xy;
}

struct xy pane_scale(struct pane *p safe)
{
	/* "scale" is roughly pixels-per-point * 1000
	 * So 10*scale.x/100 is the width of a typical character in default font.
	 * 10*scale.y/100 is the height.
	 * scale.x should be passed to Draw:text-size and and Draw:text to get
	 * correctly sized text
	 *
	 * "scale:M" is provided by the display module and reports
	 * the size of a captial M in default fond - width and height in pixels
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
	/* "scale" is a request to change from the default.
	 * It can be a simple number, in which case it is 1000 times a scale
	 * factor, so "500" would be half of default size.
	 * It can be an x,y pair, e.g. "800x240".
	 * This chooses a scale so that the given number of points, (1/10 of
	 * size of default "M") will fit in the pane.  If the pane is resized,
	 * the scale will automatically adjust to fit the requested number
	 * of characters.
	 */
	sc = pane_attr_get(p, "scale");
	if (sc == NULL)
		scale = 1000;
	else if (sscanf(sc, "%dx%d", &w, &h) == 2) {
		/* choose scale so w,h point fits in pane */
		int xscale, yscale;
		if (w <= 0) w = 1;
		if (h <= 0) h = 1;
		xscale = 1000 * p->w * 10 / mw / w;
		yscale = 1000 * p->h * 10 / mh / h;
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

static inline unsigned int ts_to_ms(struct timespec *ts safe)
{
	return ts->tv_nsec / 1000 / 1000 + ts->tv_sec * 1000;
}

bool pane_too_long(struct pane *p safe, unsigned int msec)
{
	struct timespec ts;
	unsigned int duration;

	if (p->timestamp == 0)
		return False;
	if (p->timestamp == 1)
		return True;
	clock_gettime(CLOCK_MONOTONIC_COARSE, &ts);
	duration = ts_to_ms(&ts) - p->timestamp;
	if (msec < 100)
		msec = 100;
	if (duration <= msec)
		return False;
	/* If running under gdb, then I was probaly delayed
	 * by single-stepping, so don't through an error
	 */
	p->timestamp = ! debugger_is_present();
	return p->timestamp;
}

void pane_set_time(struct pane *p safe)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC_COARSE, &ts);
	p->timestamp = ts_to_ms(&ts);
	if (p->timestamp <= 1)
		p->timestamp = 2;
}

DEF_CB(clear_consistency)
{
	ci->focus->consistency_checks = 0;
	return 1;
}

bool pane_no_consistency(struct pane *p safe)
{
	p->consistency_checks += 1;
	if (p->consistency_checks == 50)
		call_comm("event:on-idle", p, &clear_consistency, 2);
	return p->consistency_checks > 60;
}
