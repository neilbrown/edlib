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

#include "list.h"
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
}

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

static void __pane_refresh(struct pane *p, int damage)
{
	struct pane *c;

	damage |= p->damaged;
	if (!damage)
		return;
	p->damaged = 0;
	if (damage & ~DAMAGED_CHILD) {
		if (p->refresh(p, damage))
			damage = 0;
	}
	list_for_each_entry(c, &p->children, siblings)
		__pane_refresh(c, damage);
}

void pane_refresh(struct pane *p)
{
	__pane_refresh(p, 0);
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
	struct pane *c;
	if (p->z > z) {
		if (w &&
		    x < p->x + p->w &&
		    x + *w > p->x + p->w)
			*w = p->x + p->w - x;
		if (h &&
		    y < p->y + p->h &&
		    y + *h > p->y + p->h)
			*h = p->y + p->h - x;
		if (p->x > x || p->x + p->w < x)
			return 0;
		if (p->y > y || p->y + p->h < y)
			return 0;
	}
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
			*h = p->h -= *y;
		*x += p->x;
		*y += p->y;
		if (!p->parent)
			return p;
		p = p->parent;
	}
}

void pane_focus(struct pane *p)
{
	while (p->parent) {
		p->parent->focus = p;
		p = p->parent;
	}
	pane_damaged(p, DAMAGED_CURSOR);
}
