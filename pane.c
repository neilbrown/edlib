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
#include "keymap.h"

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
	p->cx = p->cy = 0;
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
			   refresh_fn refresh, void *data, struct list_head *here)
{
	struct pane *p = malloc(sizeof(*p));
	pane_init(p, parent, here);
	if (parent)
		p->z = parent->z + z;
	else
		p->z = z;
	p->refresh = refresh;
	p->data = data;
	return p;
}

static void __pane_refresh(struct pane *p, struct pane *point_pane, int damage)
{
	struct pane *c;

	if (p->point)
		point_pane = p;
	damage |= p->damaged;
	if (!damage)
		return;
	if (damage == DAMAGED_CHILD)
		damage = 0;
	else
		damage = p->refresh(p, point_pane, damage) | (damage & DAMAGED_FORCE);
	p->damaged = 0;
	list_for_each_entry(c, &p->children, siblings)
		__pane_refresh(c, point_pane, damage);
}

void pane_refresh(struct pane *p)
{
	pane_damaged(p, DAMAGED_CURSOR);
	__pane_refresh(p, NULL, 0);
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

void pane_reparent(struct pane *p, struct pane *newparent, struct list_head *here)
{
	list_del(&p->siblings);
	if (p->parent->focus == p)
		p->parent->focus = NULL;
	p->parent = newparent;
	if (!here)
		here = &newparent->children;
	list_add(&p->siblings, here);
}

void pane_free(struct pane *p)
{
	ASSERT(!list_empty(&p->children));
	list_del(&p->siblings);
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
