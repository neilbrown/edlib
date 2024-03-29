/*
 * Copyright Neil Brown ©2015-2023 <neil@brown.name>
 * May be distributed under terms of GPLv2 - see file:COPYING
 *
 * Tile manager for edlib.
 *
 * Given a display pane, tile it with other panes which will be
 * used by some other clients, probably text buffers.
 * The owner of a pane can:
 *  - split it: above/below/left/right,
 *  - destroy it
 *  - add/remove lines above/below/left/right
 *
 * Child panes are grouped either in rows or columns.  Those panes
 * can then be subdivided further.
 */

#include <unistd.h>
#include <stdlib.h>
#include <string.h>

/* The data can move between panes, so we must use a PTR type */
#define PANE_DATA_PTR_TYPE struct tileinfo *
struct tileinfo;
#include "core.h"

struct tileinfo {
	/* If direction is Horiz, this and siblings are stacked
	 * left to right.  Y co-ordinate is zero.
	 * If direction is Vert, siblings are stacked top to bottom.
	 * X co-ordinate is zero.
	 * The root of a tree of panes has direction of Neither.  All
	 * other panes are either Horiz or Vert.
	 *
	 * avail_inline is how much this tile can shrink in the direction
	 * of stacking.  Add these for parent.
	 * avail_perp is how much this tile can shrink perpendicular to direction.
	 * Min of these applies to parent.
	 */
	enum dir {Neither, Horiz, Vert}	direction;
	short				avail_inline;
	short				avail_perp;
	short				leaf;
	struct list_head		tiles; /* headless ordered list of all tiles
						* in the tree.  Used for next/prev
						*/
	struct pane			*p safe;
	struct pane			*content; /* if 'leaf' */
	char				*group; /* only allocate for root, other share */
	char				*name; /* name in group for this leaf */
};
#include "core-pane.h"

static struct map *tile_map safe;
static void tile_adjust(struct pane *p safe);
static void tile_avail(struct pane *p safe, struct pane *ignore);
static int tile_destroy(struct pane *p safe);
DEF_LOOKUP_CMD(tile_handle, tile_map);

static inline bool mine(struct pane *t safe)
{
	return t->z == 0 && t->handle == &tile_handle.c;
}

DEF_CMD_CLOSED(tile_close)
{
	struct tileinfo *ti = ci->home->data;

	tile_destroy(ci->home);
	free(ti->name);
	unalloc(ti, pane);
	return 1;
}

DEF_CMD(tile_refresh_size)
{
	struct pane *p = ci->home;
	struct tileinfo *ti = p->data;

	if (ti->direction == Neither) {
		tile_avail(p, NULL);
		tile_adjust(p);
	}
	return !ti->leaf;
}

DEF_CMD(tile_clone)
{
	struct pane *parent = ci->focus;
	struct pane *p2, *child;
	struct tileinfo *ti, *cti;

	/* Clone a new 'tile' onto the parent, but only
	 * create a single tile, cloned from the focus pane
	 */
	child = ci->home;
	cti = child->data;
	alloc(ti, pane);
	ti->leaf = 1;
	ti->direction = Neither;
	if (cti->group)
		ti->group = strdup(cti->group);
	INIT_LIST_HEAD(&ti->tiles);
	p2 = pane_register(parent, 0, &tile_handle.c, ti);
	if (!p2)
		return Efail;
	ti->p = p2;
	/* Remove borders as our children will provide their own. */
	call("Tile:border", p2);
	attr_set_str(&p2->attrs, "borders", "BL");
	while (!cti->leaf && child->focus) {
		child = child->focus;
		cti = child->data;
	}
	cti = list_next_entry(cti, tiles);
	while (cti != child->data &&
	       (cti->name == NULL || strcmp(cti->name, "main") != 0))
		cti = list_next_entry(cti, tiles);
	child = cti->p;
	if (cti->name)
		ti->name = strdup(cti->name);
	pane_clone_children(child, p2);
	return 1;
}

DEF_CMD(tile_attach)
{
	struct pane *display = ci->focus;
	struct tileinfo *ti;
	struct pane *p;

	/* Remove borders as our children will provide their own. */
	call("Tile:border", display);

	alloc(ti, pane);
	p = pane_register(display, 0, &tile_handle.c, ti);
	if (!p)
		return Efail;
	ti->leaf = 1;
	ti->p = p;
	ti->direction = Neither;
	if (ci->str)
		ti->group = strdup(ci->str);
	if (ci->str2)
		ti->name = strdup(ci->str2);
	INIT_LIST_HEAD(&ti->tiles);
	attr_set_str(&p->attrs, "borders", "BL");
	return comm_call(ci->comm2, "callback:attach", p);
}

static struct pane *tile_split(struct pane **pp safe, int horiz, int after,
			       const char *name)
{
	/* Create a new pane near the given one, reducing its size,
	 * and possibly the size of other siblings.
	 * Return a new pane which is a sibling of the old, or NULL
	 * if there is no room for further splits.
	 * This may require creating a new parent and moving 'p' down
	 * in the hierarchy.
	 */
	int space, new_space;
	struct pane *p = safe_cast *pp;
	struct pane *ret;
	struct tileinfo *ti = p->data;
	struct tileinfo *ti2;
	if (horiz)
		space = p->w;
	else
		space = p->h;

	/* FIXME ask the leafs */
	if (space < 8)
		return NULL;
	new_space = space / 2;
	space -= new_space;

	if (ti->direction != (horiz? Horiz : Vert)) {
		/* This tile does not stack in the required direction, need
		 * to create an extra level.
		 */
		struct pane *p2, *child;
		struct list_head *t;
		/* ti2 will be tileinfo for p, new pane gets ti */
		alloc(ti2, pane);
		ti2->leaf = 0;
		ti2->direction = ti->direction;
		ti2->group = ti->group;
		INIT_LIST_HEAD(&ti2->tiles);
		p->data = ti2;
		ti2->p = p;
		p2 = pane_register(p, 0, &tile_handle.c, ti);
		if (!p2)
			return NULL;
		ti->p = p2;
		ti->direction = horiz ? Horiz : Vert;
		/* All children of p must be moved to p2, except p2 */
		list_for_each_entry_safe(child, t, &p->children, siblings)
			if (child != p2)
				pane_reparent(child, p2);
		p = p2;
	}
	alloc(ti2, pane);
	ti2->group = ti->group;
	ti2->direction = ti->direction;
	ti2->leaf = 1;
	if (name)
		ti2->name = strdup(name);
	/* FIXME if ti wasn't a leaf, this is wrong.  Is that possible? */
	if (after)
		list_add(&ti2->tiles, &ti->tiles);
	else
		list_add_tail(&ti2->tiles, &ti->tiles);
	ret = pane_register(p->parent, 0, &tile_handle.c, ti2);
	if (!ret)
		return NULL;

	ti2->p = ret;
	if (after)
		pane_move_after(ret, p);
	else if (p == list_first_entry(&p->parent->children,
				       struct pane, siblings))
		pane_move_after(ret, NULL);
	else
		pane_move_after(ret, list_prev_entry(p, siblings));
	switch (!!horiz + 2 * !!after) {
	case 0: /* vert before */
		pane_resize(ret, p->x, p->y, p->w, new_space);
		pane_resize(p, p->x, p->y + ret->h, p->w, space);
		break;
	case 1: /* horiz before */
		pane_resize(ret, p->x, p->y, new_space, p->h);
		pane_resize(p, p->x + ret->w, p->y, space, p->h);
		break;
	case 2: /* vert after */
		pane_resize(ret, p->x, p->y + space, p->w, new_space);
		pane_resize(p, p->x, p->y, p->w, space);
		break;
	case 3: /* horiz after */
		pane_resize(ret, p->x + space, p->y, new_space, p->h);
		pane_resize(p, p->x, p->y, space, p->h);
		break;
	}
	tile_adjust(ret);
	tile_adjust(p);
	*pp = p;
	return ret;
}

static int tile_destroy(struct pane *p safe)
{
	struct tileinfo *ti = p->data;
	struct pane *prev = NULL, *next = NULL;
	struct pane *t, *remain = NULL;
	int pos, prevpos, nextpos;
	int remaining = 0;

	if (ti->direction == Neither /* Root file a tile-set */
	    || p->parent == p /* subsumbed hust being destroyed */
	    || p->parent->handle != p->handle /* Some messed with parentage */
	)
		return 1;

	if (ti->direction == Vert)
		pos = p->y;
	else
		pos = p->x;
	prevpos = nextpos = -1;
	list_for_each_entry(t, &p->parent->children, siblings) {
		int pos2;
		if (t->z)
			continue;
		if (ti->direction == Vert)
			pos2 = t->y;
		else
			pos2 = t->x;
		if (pos2 < pos && (prev == NULL || prevpos < pos2))
			prev = t;
		if (pos2 > pos && (next == NULL || nextpos > pos2))
			next = t;

		remaining += 1;
		remain = t;
	}
	/* There is always a sibling of a non-root */
	//ASSERT(remaining > 0);
	if (prev == NULL /* FIXME redundant */ && next) {
		/* next gets the space and focus*/
		if (ti->direction == Horiz)
			pane_resize(next, p->x, next->y,
				    p->w + next->w, next->h);
		else
			pane_resize(next, next->x, p->y,
				    next->w, p->h + next->h);
		tile_adjust(next);
		p->parent->focus = next;
	} else if (next == NULL /* FIXME redundant */ && prev) {
		/* prev gets the space and focus */
		if (ti->direction == Horiz)
			pane_resize(prev, prev->x, prev->y,
				    prev->w + p->w, prev->h);
		else
			pane_resize(prev, prev->x, prev->y,
				    prev->w, prev->h + p->h);
		tile_adjust(prev);
		p->parent->focus = prev;
	} else if (/*FIXME*/ next && prev) {
		/* space to the smallest, else share the space */
		/* Focus goes to prev, unless next is small */
		p->parent->focus = prev;
		if (ti->direction == Horiz) {
			int w = p->w / 2;
			if (prev->w < next->w*2/3)
				/* prev is much smaller, it gets all */
				w = p->w;
			else if (next->w < prev->w*2/3) {
				w = 0;
				p->parent->focus = next;
			}
			pane_resize(prev, prev->x, prev->y, prev->w + w, prev->h);
			w = p->w - w;
			pane_resize(next, prev->x + prev->w, next->y,
				    next->w + w, next->h);
		} else {
			int h = p->h / 2;
			if (prev->h < next->h*2/3)
				/* prev is much smaller, it gets all */
				h = p->h;
			else if (next->h < prev->h*2/3) {
				h = 0;
				p->parent->focus = next;
			}
			pane_resize(prev, prev->x, prev->y, prev->w, prev->h + h);
			h = p->h - h;
			pane_resize(next, next->x, prev->y + prev->h,
				    next->w , next->h + h);
		}
		tile_adjust(next);
		tile_adjust(prev);
	}
	list_del(&ti->tiles);
	if (remaining == 1 && remain && remain->parent != remain &&
	    remain->handle == p->handle) {
		struct tileinfo *ti2;
		enum dir tmp;
		/* Only one child left, must move it into parent.
		 * Cannot destroy the parent, so bring child into parent */
		p = remain->parent;

		ti = remain->data;
		ti2 = p->data;

		tmp = ti2->direction;
		ti2->direction = ti->direction;
		ti->p = p;
		ti->direction = tmp;
		ti2->p = remain;

		pane_subsume(remain, p);
	}
	return 1;
}

static void tile_avail(struct pane *p safe, struct pane *ignore)
{
	/* How much can we shrink this pane?
	 * If 'ignore' is set, it is a child of 'p', and we only
	 * consider other children.
	 * If stacking direction matches 'horiz' find sum of avail in children.
	 * if stacking direction doesn't match 'horiz', find minimum.
	 * If only one child, assume min of 4.
	 */
	struct tileinfo *ti = p->data;
	struct pane *t;

	if (ti->leaf) {
		/* one or zero children */
		if (ti->direction == Horiz) {
			ti->avail_inline = p->w < 4 ? 0 : p->w - 4;
			ti->avail_perp = p->h < 4 ? 0 : p->h - 4;
		} else {
			ti->avail_inline = p->h < 4 ? 0 : p->h - 4;
			ti->avail_perp = p->w < 4 ? 0 : p->w - 4;
		}
	} else {
		struct tileinfo *ti2;
		int sum = 0, min = -1;
		list_for_each_entry(t, &p->children, siblings) {
			if (t == ignore || !mine(t))
				continue;
			tile_avail(t, NULL);
			ti2 = t->data;
			if (min < 0 || min > ti2->avail_perp)
				min = ti2->avail_perp;
			sum += ti2->avail_inline;
		}
		ti->avail_perp = sum;
		ti->avail_inline = min;
	}
}

static void tile_adjust(struct pane *p safe)
{
	/* Size of pane 'p' has changed and it is time to adjust
	 * the children, both their size and offset.
	 * For the non-stacking direction, offset must be zero and
	 * size is copied from p.
	 * For the stacking direction, we count used size, find the
	 * change required and distribute that.  For shrinking, this
	 * relies on the fact that 'tile_avail' must have been run
	 * and it left avail space calculations around.
	 */
	struct pane *t;
	int used = 0;
	int cnt = 0;
	int avail_cnt = 0;
	int pos;
	int size = 0;
	struct tileinfo *ti = p->data;

	if (ti->leaf)
		/* Children are responsible for themselves. */
		return;

	list_for_each_entry(t, &p->children, siblings) {

		if (!mine(t))
			continue;
		ti = t->data;
		if (ti->direction == Horiz) {
			pane_resize(t, t->x, 0, t->w, p->h);
			used += t->w;
			size = p->w;
		} else {
			pane_resize(t, 0, t->y, p->w, t->h);
			used += t->h;
			size = p->h;
		}
		if (ti->avail_inline)
			avail_cnt++;
		cnt++;
	}
	while (used < size || (used > size && avail_cnt)) {
		int change = 0;
		int remain = used; /* size of panes still to be resized */

		if (used > size)
			cnt = avail_cnt;
		avail_cnt = 0;
		list_for_each_entry(t, &p->children, siblings) {
			struct tileinfo *ti2 = t->data;
			int diff;
			int mysize;
			if (!mine(t))
				continue;
			if (!remain)
				break;
			mysize = (ti2->direction == Horiz) ? t->w : t->h;

			if (used > size) {
				/* shrinking */
				if (ti2->avail_inline == 0) {
					remain -= mysize;
					continue;
				}
				diff = (((used - size) * mysize) +
					(used%remain)) / remain;
				if (diff > ti2->avail_inline)
					diff = ti2->avail_inline;
				ti2->avail_inline -= diff;
				if (ti2->avail_inline)
					/* Still space available if needed */
					avail_cnt++;

				diff = -diff;
			} else if (used == size)
				break;
			else
				diff = (((size - used) * mysize) +
					(used%remain) )/ remain;
			remain -= mysize;
			if (diff)
				change = 1;
			if (ti2->direction == Horiz)
				pane_resize(t, t->x, t->y, t->w + diff, t->h);
			 else
				pane_resize(t, t->x, t->y, t->w, t->h + diff);

			used += diff;
			cnt--;
		}
		if (!change)
			break;
	}
	pos = 0;
	list_for_each_entry(t, &p->children, siblings) {
		struct tileinfo *ti2 = t->data;
		if (!mine(t))
			continue;
		if (ti2->direction == Horiz) {
			pane_resize(t, pos, t->y, t->w, t->h);
			pos += t->w;
		} else {
			pane_resize(t, t->x, pos, t->w, t->h);
			pos += t->h;
		}
		tile_adjust(t);
	}
}

static bool tile_grow(struct pane *p safe, int horiz, int size)
{
	/* Try to grow the pane in given direction, or shrink if
	 * size < 0.
	 * This is only done by shrinking other tiles, not by
	 * resizing the top level.
	 * If this pane isn't stacked in the right direction, or if
	 * neighbors are too small to shrink, the parent is resized.
	 * Then that propagates back down.  Size of this pane is adjusted
	 * first to catch the propagations, then corrected after.
	 */
	struct tileinfo *ti = p->data;
	struct tileinfo *tip;
	int avail;

	if (ti->direction == Neither)
		/* Cannot grow/shrink the root */
		return False;
	if (size < 0) {
		/* Does this pane have room to shrink */
		tile_avail(p, NULL);
		if (ti->direction == (horiz? Horiz : Vert))
			avail = ti->avail_inline;
		else
			avail = ti->avail_perp;
		if (avail < -size)
			return False;
	}
	if (ti->direction != (horiz ? Horiz : Vert)) {
		/* need to ask parent to do this */
		return tile_grow(p->parent, horiz, size);
	}

	/* OK, this stacks in the right direction. if shrinking we can commit */
	if (size < 0) {
		struct pane *other = NULL;
		struct pane *t;
		int p_found = 0;
		list_for_each_entry(t, &p->parent->children, siblings) {
			if (!mine(t))
				continue;
			if (t == p)
				p_found = 1;
			else
				other = t;
			if (other && p_found)
				break;
		}

		if (other == NULL)
			/* Strange - there should have been two elements in list */
			return True;
		if (ti->direction == Horiz) {
			pane_resize(p, p->x, p->y, p->w + size, p->h);
			pane_resize(other, other->x, other->y,
				    other->w - size, other->h);
		} else {
			pane_resize(p, p->x, p->y, p->w, p->h + size);
			pane_resize(other, other->x, other->y,
				    other->w, other->h - size);
		}
		tile_adjust(p->parent);
		return True;
	}

	/* Hoping to grow if there is room for others to shrink */
	tile_avail(p->parent, p);
	tip = p->parent->data;
	if (ti->direction == (horiz ? Horiz : Vert))
		avail = tip->avail_inline;
	else
		avail = tip->avail_perp;
	if (avail < size)
		return False;
	if (ti->direction == Horiz)
		pane_resize(p, p->x, p->y, p->w + size, p->h);
	else
		pane_resize(p, p->x, p->y, p->w, p->h + size);

	ti->avail_inline = 0; /* make sure this one doesn't suffer */
	tile_adjust(p->parent);
	return True;
}

static struct pane *next_child(struct pane *parent, struct pane *prev, bool popup)
{
	struct pane *p2;
	list_for_each_entry(p2, &parent->children, siblings) {
		if (p2 == prev) {
			prev = NULL;
			continue;
		}
		if (prev)
			continue;
		if (mine(p2) == popup)
			continue;
		return p2;
	}
	return NULL;
}

static struct tileinfo *tile_first(struct tileinfo *ti safe)
{
	while (!ti->leaf) {
		struct pane *p = next_child(ti->p, NULL, 0);
		if (!p)
			return NULL;
		ti = p->data;
	}
	return ti;
}

static bool tile_is_first(struct tileinfo *ti safe)
{
	while (ti->direction != Neither) {
		if (ti->p != next_child(ti->p->parent, NULL, 0))
			return False;
		ti = ti->p->parent->data;
	}
	return True;
}

static struct pane *tile_root_popup(struct tileinfo *ti safe)
{
	while (ti->direction != Neither)
		ti = ti->p->parent->data;
	return next_child(ti->p, NULL, 1);
}

static struct tileinfo *safe tile_next_named(struct tileinfo *ti safe,
					     const char *name)
{
	struct tileinfo *t = ti;
	while ((t = list_next_entry(t, tiles)) != ti) {
		if (!name)
			return t;
		if (!t->name || strcmp(t->name, name) != 0)
			continue;
		return t;
	}
	return t;
}

static bool wrong_pane(struct cmd_info const *ci safe)
{
	struct tileinfo *ti = ci->home->data;

	if (ci->str || ti->group) {
		if (!ci->str || !ti->group)
			return True;
		if (strcmp(ci->str, ti->group) != 0)
			return True;
		/* same group - continue */
	}
	return False;
}

DEF_CMD(tile_window_next)
{
	/* If currently on a popup, go to next popup if there is one, else
	 * to this tile.
	 * If was not on a pop-up, go to next tile and if there is a popup,
	 * go there.
	 */
	struct pane *p = ci->home;
	struct pane *p2;
	struct tileinfo *ti = p->data;
	struct tileinfo *t2;

	if (wrong_pane(ci))
		return Efallthrough;
	if (p->focus && p->focus->z) {
		p2 = next_child(p, p->focus, 1);
		if (p2) {
			pane_take_focus(p2);
			return 1;
		} else if (ti->leaf) {
			pane_take_focus(ti->content);
			return 1;
		}
		t2 = tile_first(ti);
	} else {
		if (ti->leaf) {
			t2 = tile_next_named(ti, ci->str2);
			if (tile_is_first(t2) &&
			    (p2 = tile_root_popup(t2)) != NULL) {
				pane_take_focus(p2);
				return 1;
			}
		} else
			t2 = tile_first(ti);
	}
	if (t2) {
		pane_take_focus(t2->p);
		p2 = next_child(t2->p, NULL, 1);
		if (p2)
			pane_take_focus(p2);
	}
	return 1;
}

DEF_CMD(tile_window_prev)
{
	struct pane *p = ci->home;
	struct tileinfo *ti = p->data;
	struct tileinfo *t2;

	if (wrong_pane(ci))
		return Efallthrough;
	t2 = list_prev_entry(ti, tiles);
	pane_take_focus(t2->p);
	return 1;
}

DEF_CMD(tile_window_xplus)
{
	struct pane *p = ci->home;

	if (wrong_pane(ci))
		return Efallthrough;
	tile_grow(p, 1, RPT_NUM(ci));
	return 1;
}

DEF_CMD(tile_window_xminus)
{
	struct pane *p = ci->home;

	if (wrong_pane(ci))
		return Efallthrough;
	tile_grow(p, 1, -RPT_NUM(ci));
	return 1;
}
DEF_CMD(tile_window_yplus)
{
	struct pane *p = ci->home;

	if (wrong_pane(ci))
		return Efallthrough;
	tile_grow(p, 0, RPT_NUM(ci));
	return 1;
}
DEF_CMD(tile_window_yminus)
{
	struct pane *p = ci->home;

	if (wrong_pane(ci))
		return Efallthrough;
	tile_grow(p, 0, -RPT_NUM(ci));
	return 1;
}

DEF_CMD(tile_window_splitx)
{
	struct pane *p = ci->home;
	struct pane *p2;

	if (wrong_pane(ci))
		return Efallthrough;
	p2 = tile_split(&p, 1, 1, ci->str2);
	pane_clone_children(p, p2);
	return 1;
}

DEF_CMD(tile_window_splity)
{
	struct pane *p = ci->home;
	struct pane *p2;

	if (wrong_pane(ci))
		return Efallthrough;
	p2 = tile_split(&p, 0, 1, ci->str2);
	pane_clone_children(p, p2);
	return 1;
}

DEF_CMD(tile_window_close)
{
	struct pane *p = ci->home;
	struct tileinfo *ti = p->data;

	if (wrong_pane(ci))
		return Efallthrough;
	if (ti->direction != Neither)
		pane_close(p);
	return 1;
}

DEF_CMD(tile_window_bury)
{
	/* Bury the document in this tile.
	 * Find some other document to display
	 */
	struct pane *doc;

	if (wrong_pane(ci))
		return Efallthrough;

	/* First, push the doc to the end of the 'recently used' list */
	call("doc:notify:doc:revisit", ci->focus, -1);
	/* Now choose a replacement */
	doc = call_ret(pane, "docs:choose", ci->home);
	if (doc)
		/* display that doc in this pane */
		home_call(doc, "doc:attach-view", ci->home);
	return 1;
}

DEF_CMD(tile_window_close_others)
{
	struct pane *p = ci->home;
	struct pane *parent = p->parent;
	struct tileinfo *ti = p->data;
	bool found =  True;

	if (wrong_pane(ci))
		return Efallthrough;
	/* close sibling panes until ->parent changes, or there aren't any */
	while (found && p->parent == parent) {
		struct pane *s;

		found = False;
		list_for_each_entry(s, &parent->children, siblings)
			if (s != p) {
				found = True;
				pane_close(s);
				break;
			}
	}
	return ti->direction != Neither ? 1 : Efalse;
}

DEF_CMD(tile_other)
{
	/* Choose some other tile.  If there aren't any, make one.
	 * Result is returned in ci->focus
	 * ci->num has flags:
	 *  1: if split is need, use 2 to determine direction, else default
	 *  2: if split needed, split horizontally, else vertically
	 *  4: if split needed use 8 to determine which is new, else default
	 *  8: if split is needed, new pane is to the right/down.
	 *  512: don't split, just return Efalse
	 */
	struct pane *p = ci->home;
	struct pane *p2;
	struct tileinfo *ti = p->data;
	struct tileinfo *ti2;
	int horiz, after;

	if (ci->str || ti->group) {
		if (!ci->str || !ti->group)
			return Efallthrough;
		if (strcmp(ci->str, ti->group) != 0)
			return Efallthrough;
		/* same group - continue */
	}
	if (!ti->leaf) {
		/* probably coming from a pop-up. Just use first tile */
		ti2 = tile_first(ti);
		if (!ti2)
			return Einval;
		if (ci->str2 && ti2->name && strcmp(ci->str2, ti2->name) == 0)
			return Einval;
		return comm_call(ci->comm2, "callback:pane", ti2->p);
	}
	if (ci->str2 && ti->name && strcmp(ci->str2, ti->name) == 0)
		return Einval;

	ti2 = tile_next_named(ti, ci->str2);
	if (ti2 != ti)
		return comm_call(ci->comm2, "callback:pane", ti2->p);

	/* Need to create a tile.  If wider than 120 (FIXME configurable?),
	 * horiz-split else vert
	 */
	if (ci->num & 512)
		return Efalse;

	if (ci->num & 1) {
		horiz = ci->num & 2;
	} else {
		struct xy xy = pane_scale(p);
		horiz = p->w * 1000 >= 1200 * xy.x;
	}
	if (ci->num & 4)
		after = ci->num & 8;
	else
		after = 1;
	p2 = tile_split(&p, horiz, after, ci->str2);
	if (p2)
		return comm_call(ci->comm2, "callback:pane", p2);
	return Efail;
}

DEF_CMD(tile_this)
{
	struct tileinfo *ti = ci->home->data;

	if (ci->str || ti->group) {
		if (!ci->str || !ti->group)
			return Efallthrough;
		if (strcmp(ci->str, ti->group) != 0)
			return Efallthrough;
		/* same group - continue */
	}
	if (!ti->leaf) {
		/* There is no clear 'This', use first. */
		ti = tile_first(ti);
		if (!ti)
			return Einval;
		if (ci->str2 && ti->name && strcmp(ci->str2, ti->name) == 0)
			return Einval;
		return comm_call(ci->comm2, "callback:pane", ti->p);
	}
	return comm_call(ci->comm2, "callback:pane", ci->home, 0,
			 NULL, ti->name);
}

DEF_CMD(tile_doc)
{
	/* Find the pane displaying given document, preferrably not
	 * this pane
	 */
	struct tileinfo *ti = ci->home->data;
	struct tileinfo *t;
	char *name;

	if (ci->str || ti->group) {
		if (!ci->str || !ti->group)
			return Efallthrough;
		if (strcmp(ci->str, ti->group) != 0)
			return Efallthrough;
		/* same group - continue */
	}
	/* Find where 'focus' is open */
	name = pane_attr_get(ci->focus, "doc-name");
	if (!name)
		return Efallthrough;
	if (!ti->leaf)
		ti = tile_first(ti);
	t = ti;
	do {
		char *n;
		struct pane *f;
		t = list_next_entry(t, tiles);
		f = t->content;
		if (f) {
			f = pane_leaf(f);
			n = pane_attr_get(f, "doc-name");
			if (n && strcmp(n, name) == 0)
				return comm_call(ci->comm2, "callback:pane",
						 t->p,
						 0, NULL, t->name);
		}
	} while (t != ti);

	return Efallthrough;
}

DEF_CMD(tile_root)
{
	struct pane *p = ci->home;
	struct tileinfo *ti = p->data;

	if (ti->direction != Neither)
		return Efallthrough;
	if (ci->str || ti->group) {
		if (!ci->str || !ti->group)
			return Efallthrough;
		if (strcmp(ci->str, ti->group) != 0)
			return Efallthrough;
		/* same group - continue */
	}

	return comm_call(ci->comm2, "callback:pane", p);
}

DEF_CMD(tile_child_notify)
{
	struct pane *p = ci->home;
	struct tileinfo *ti = p->data;
	struct pane *c = ci->focus;

	if (c->z)
		return 1;
	if (ci->num > 0 && mine(c))
		/* always accept my own children */
		return 1;

	if (ti->leaf != 1) {
		if (ci->num > 0)
			/* Sorry, new children not permitted */
			return Efalse;
		return 1;
	}

	switch (ci->num) {
	case -1:
		/* Child closed, but we weren't, so find something else to display */
		ti->content = NULL;
		c = call_ret(pane, "docs:choose", p);
		if (c)
			home_call(c, "doc:attach-view", p);
		else if (ti->direction != Neither)
			pane_close(p);
		break;
	case 1:
		/* New pane, discard the old */
		p->focus = c;
		if (ti->content) {
			ti->leaf = 2;
			pane_close(ti->content);
			ti->leaf = 1;
		}
		ti->content = c;
		break;
	case -2:
		/* Child moved away - hopefully to be replaced */
		ti->content = NULL;
		break;
	case 2:
		/* Simple replacement */
		ti->content = c;
		break;
	}
	return 1;
}

void edlib_init(struct pane *ed safe)
{
	tile_map = key_alloc();

	key_add(tile_map, "Tile:next", &tile_window_next);
	key_add(tile_map, "Tile:prev", &tile_window_prev);
	key_add(tile_map, "Tile:x+", &tile_window_xplus);
	key_add(tile_map, "Tile:x-", &tile_window_xminus);
	key_add(tile_map, "Tile:y+", &tile_window_yplus);
	key_add(tile_map, "Tile:y-", &tile_window_yminus);
	key_add(tile_map, "Tile:split-x", &tile_window_splitx);
	key_add(tile_map, "Tile:split-y", &tile_window_splity);
	key_add(tile_map, "Tile:close", &tile_window_close);
	key_add(tile_map, "Tile:close-others", &tile_window_close_others);
	key_add(tile_map, "Tile:bury", &tile_window_bury);

	key_add(tile_map, "OtherPane", &tile_other);
	key_add(tile_map, "ThisPane", &tile_this);
	key_add(tile_map, "DocPane", &tile_doc);
	key_add(tile_map, "RootPane", &tile_root);
	key_add(tile_map, "Clone", &tile_clone);
	key_add(tile_map, "Child-Notify", &tile_child_notify);
	key_add(tile_map, "Close", &tile_close);
	key_add(tile_map, "Refresh:size", &tile_refresh_size);

	call_comm("global-set-command", ed, &tile_attach, 0, NULL, "attach-tile");
}
