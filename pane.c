/*
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

#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <ctype.h>

#include "core.h"
#include "pane.h"
#include "tile.h"

static void pane_init(struct pane *p, struct pane *par, struct list_head *here)
{
	p->parent = par;
	if (par && !here)
		here = &par->children;
	if (here)
		list_add(&p->siblings, here);
	else
		INIT_LIST_HEAD(&p->siblings);
	INIT_LIST_HEAD(&p->children);
	p->x = p->y = p->z = 0;
	p->cx = p->cy = -1;
	p->h = p->w = 0;
	p->focus = NULL;
	p->refresh = NULL;
	p->data = NULL;
	p->damaged = 0;
	p->point = NULL;
	p->keymap = NULL;
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
		type = DAMAGED_CHILD | (type & DAMAGED_CURSOR);
		p = p->parent;
	}
}

struct pane *pane_register(struct pane *parent, int z,
			   struct command *refresh, void *data, struct list_head *here)
{
	struct pane *p = malloc(sizeof(*p));
	pane_init(p, parent, here);
	if (parent)
		p->z = parent->z + z;
	else
		p->z = z;
	p->refresh = refresh;
	p->data = data;
	if (parent && parent->focus == NULL)
		parent->focus = p;
	return p;
}

static void __pane_refresh(struct cmd_info *ci)
{
	struct pane *c;
	int damage = ci->extra;
	struct pane *p = ci->home;
	struct pane *pp;

	if (p->point)
		ci->point_pane = p;
	pp = ci->point_pane;
	damage |= p->damaged;
	if (!damage)
		return;
	if (damage == DAMAGED_CHILD)
		damage = 0;
	else {
		ci->extra = damage;
		damage &= DAMAGED_FORCE | DAMAGED_SIZE | DAMAGED_CURSOR;
		if (p->refresh->func(p->refresh, ci))
			damage |= ci->extra;
	}
	p->damaged = 0;
	list_for_each_entry(c, &p->children, siblings) {
		ci->point_pane = pp;
		ci->extra = damage;
		ci->home = c;
		__pane_refresh(ci);
	}
}

void pane_refresh(struct pane *p)
{
	struct cmd_info ci = {0};
	pane_damaged(p, DAMAGED_CURSOR);
	ci.focus = ci.home = p;
	ci.key = "Refresh";
	__pane_refresh(&ci);
}

void pane_close(struct pane *p)
{
	struct cmd_info ci = {0};
	struct pane *c, *pp;

	while (!list_empty(&p->children)) {
		c = list_first_entry(&p->children, struct pane, siblings);
		pane_close(c);
	}
	ci.key = "Close";
	ci.focus = ci.home = p;
	pp = p;
	while (pp && !pp->point)
		pp = pp->parent;
	ci.point_pane = pp;
	list_del_init(&p->siblings);
	if (p->refresh)
		p->refresh->func(p->refresh, &ci);
	pane_damaged(p->parent, DAMAGED_FORCE|DAMAGED_CURSOR);
/* FIXME who destroys 'point'*/
	pane_free(p);
}

int pane_clone(struct pane *from, struct pane *parent)
{
	/* Create a clone of 'from' as a child of 'parent'.
	 * We send a 'Clone' message to 'from' which may
	 * well call pane_clone() recursively on children.
	 * 'parent' is passed as the 'point_pane', which is a
	 * white lie (hopefully it remains white).
	 */
	struct cmd_info ci = {0};

	if (!from || !parent)
		return 0;
	ci.key = "Clone";
	ci.focus = ci.home = from;
	ci.point_pane = parent;
	if (from->refresh)
		return from->refresh->func(from->refresh, &ci);
	return 0;
}

void pane_resize(struct pane *p, int x, int y, int w, int h)
{
	int damage = 0;
	if (x >= 0 &&
	    (p->x != x || p->y != y)) {
		damage |= DAMAGED_POSN;
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

void pane_reparent(struct pane *p, struct pane *newparent, struct list_head *here)
{
	list_del(&p->siblings);
	if (p->parent->focus == p)
		p->parent->focus = newparent;
	p->parent = newparent;
	if (newparent->focus == NULL)
		newparent->focus = p;
	if (!here)
		here = &newparent->children;
	list_add(&p->siblings, here);
}

void pane_subsume(struct pane *p, struct pane *parent)
{
	/* move all content from p into parent, which must be empty,
	 * except possibly for 'p'.
	 * 'data', 'point' and 'refresh' are swapped.
	 * After this, p can be freed
	 */
	void *data;
	struct command *refresh;
	struct point *point;
	struct pane *c;

	list_del_init(&p->siblings);
	while (!list_empty(&p->children)) {
		c = list_first_entry(&p->children, struct pane, siblings);
		list_move(&c->siblings, &parent->children);
		c->parent = parent;
	}
	parent->focus = p->focus;
	parent->keymap = p->keymap;

	refresh = parent->refresh;
	parent->refresh = p->refresh;
	p->refresh = refresh;

	data = parent->data;
	parent->data = p->data;
	p->data = data;

	point = parent->point;
	parent->point = p->point;
	p->point = point;
	if (parent->point)
		parent->point->owner = parent;
	if (p->point)
		p->point->owner = p;
}

void pane_free(struct pane *p)
{
	ASSERT(list_empty(&p->children));
	list_del_init(&p->siblings);
	if (p->parent->focus == p)
		p->parent->focus = NULL;
	free(p);
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

	if (x > p->x+p->w || y > p->y+p->h)
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
		if (w)
			*w = p->x - x;
		if (h)
			*h = p->y - y;

		return 0;
	}
	/* This pane doesn't mask (same z level) but a child still could */
	x -= p->x;
	y -= p->y;
	list_for_each_entry(c, &p->children, siblings)
		if (pane_masked(c, x, y, z, w, h))
			return 1;

	return 0;
}

struct pane *pane_to_root(struct pane *p, int *x, int *y, int *w, int *h)
{
	while(1) {
		if (w && *x + *w > p->w)
			*w = p->w - *x;
		if (h && *y + *h > p->h)
			*h = p->h - *y;
		*x += p->x;
		*y += p->y;
		if (!p->parent)
			return p;
		p = p->parent;
	}
}

void pane_focus(struct pane *p)
{
	pane_damaged(p, DAMAGED_CURSOR);
	while (p->parent) {
		p->parent->focus = p;
		p = p->parent;
	}
}

struct editor *pane2ed(struct pane *p)
{
	struct display *dpy;
	while (p->parent)
		p = p->parent;
	dpy = p->data;
	return dpy->ed;
}

struct pane *pane_with_cursor(struct pane *p, int *oxp, int *oyp)
{
	struct pane *ret = p;
	int ox = 0, oy = 0;
	if (oxp) {
		*oxp = 0;
		*oyp = 0;
	}
	while (p) {
		ox += p->x;
		oy += p->y;

		if (p->cx >= 0 && p->cy >= 0) {
			ret = p;
			if (oxp) {
				*oxp = ox;
				*oyp = oy;
			}
		}
		p = p->focus;
	}
	return ret;
}
