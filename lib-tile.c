/*
 * Copyright Neil Brown ©2015 <neil@brown.name>
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
	enum {Neither, Horiz, Vert}	direction;
	short				avail_inline;
	short				avail_perp;
	short				leaf;
	struct list_head		tiles; /* headless ordered list of all tiles
						* in the tree.  Used for next/prev
						*/
	struct pane			*p;
	struct pane			*content; /* if 'leaf' */
};

static struct map *tile_map;
static void tile_adjust(struct pane *p);
static void tile_avail(struct pane *p, struct pane *ignore);
static int tile_destroy(struct pane *p);

DEF_CMD(tile_handle)
{
	struct pane *p = ci->home;
	struct tileinfo *ti = p->data;
	int ret;

	ret = key_lookup(tile_map, ci);
	if (ret)
		return ret;

	if (strcmp(ci->key, "Close") == 0) {
		tile_destroy(p);
		return 0;
	}

	if (strcmp(ci->key, "Refresh:size") == 0) {
		if (ti->direction == Neither) {
			pane_check_size(p);
			tile_avail(p, NULL);
			tile_adjust(p);
		}
		return 1;
	}
	return 0;
}

DEF_CMD(tile_clone)
{
	struct pane *parent = ci->focus;
	struct pane *p2, *child;
	struct tileinfo *ti;

	/* Clone a new 'tile' onto the parent, but only
	 * create a single tile, cloned from the focus pane
	 */
	ti = calloc(1, sizeof(*ti));
	ti->leaf = 1;
	ti->direction = Neither;
	INIT_LIST_HEAD(&ti->tiles);
	ti->p = p2 = pane_register(parent, 0, &tile_handle, ti, NULL);
	attr_set_str(&p2->attrs, "borders", "BL", -1);
	child = ci->home;
	ti = child->data;
	while (!ti->leaf && child->focus) {
		child = child->focus;
		ti = child->data;
	}
	pane_clone_children(child, p2);
	return 1;
}

static int get_scale(struct pane *p)
{
	char *sc = pane_attr_get(p, "scale");
	int scale;

	if (!sc)
		return 1000;

	scale = atoi(sc);
	if (scale > 3)
		return scale;
	return 1000;
}

DEF_CMD(tile_scale)
{
	struct pane *p = ci->home;
	int scale = get_scale(p);
	int rpt = RPT_NUM(ci);

	if (rpt > 10) rpt = 10;
	if (rpt < -10) rpt = -10;
	while (rpt > 0) {
		scale = scale * 11/10;
		rpt -= 1;
	}
	while (rpt < 0) {
		scale = scale * 9 / 10;
		rpt += 1;
	}

	attr_set_int(&p->attrs, "scale", scale);
	pane_damaged(p, DAMAGED_SIZE);
	return 1;
}



DEF_CMD(tile_attach)
{
	struct pane *display = ci->focus;
	struct tileinfo *ti = calloc(1, sizeof(*ti));
	struct pane *p = pane_register(display, 0, &tile_handle, ti, NULL);

	ti->leaf = 1;
	ti->p = p;
	ti->direction = Neither;
	INIT_LIST_HEAD(&ti->tiles);
	attr_set_str(&p->attrs, "borders", "BL", -1);
	return comm_call(ci->comm2, "callback:attach", p, 0, NULL, NULL, 0);
}

static struct pane *tile_split(struct pane *p, int horiz, int after)
{
	/* Create a new pane near the given one, reducing its size,
	 * and possibly the size of other siblings.
	 * Return a new pane which is a sibling of the old, or NULL
	 * if there is no room for further splits.
	 * This may require creating a new parent and moving 'p' down
	 * in the hierarchy.
	 */
	int space;
	struct pane *ret;
	struct tileinfo *ti = p->data;
	struct list_head *here;
	struct tileinfo *ti2;
	if (horiz)
		space = p->w;
	else
		space = p->h;

	/* FIXME ask the leafs */
	if (space < 8)
		return NULL;

	if (ti->direction != (horiz? Horiz : Vert)) {
		/* This tile is not split in the required direction, need
		 * to create an extra level.
		 */
		struct pane *p2;
		ti2 = calloc(1, sizeof(*ti2));
		ti2->leaf = 0;
		ti2->direction = ti->direction;
		INIT_LIST_HEAD(&ti2->tiles);
		p2 = pane_register(p->parent, 0, &tile_handle, ti2, &p->siblings);
		ti2->p = p2;
		pane_resize(p2, p->x, p->y, p->w, p->h);
		pane_reparent(p, p2);
		p2->attrs = p->attrs; p->attrs = NULL;
		pane_resize(p, 0, 0, 0, 0);
		ti->direction = horiz ? Horiz : Vert;
	}
	here = after ? &p->siblings : p->siblings.prev;
	ti2 = calloc(1, sizeof(*ti2));
	ti2->direction = ti->direction;
	ti2->leaf = ti->leaf;
	if (after)
		list_add(&ti2->tiles, &ti->tiles);
	else
		list_add_tail(&ti2->tiles, &ti->tiles);
	ret = pane_register(p->parent, 0, &tile_handle, ti2, here);
	ti2->p = ret;
	switch (!!horiz + 2 * !!after) {
	case 0: /* vert before */
		pane_resize(ret, p->x, p->y, p->w, p->h/2);
		pane_resize(p, p->x, p->y + ret->h, p->w, p->h - ret->h);
		break;
	case 1: /* horiz before */
		pane_resize(ret, p->x, p->y, p->w/2, p->h);
		pane_resize(p, p->x + ret->w, p->y, p->w - ret->w, p->h);
		break;
	case 2: /* vert after */
		pane_resize(ret, p->x, p->y + p->h/2, p->w, p->h - p->h/2);
		pane_resize(p, -1, -1, p->w, p->h/2);
		break;
	case 3: /* horiz after */
		pane_resize(ret, p->x + p->w/2, p->y, p->w - p->w/2, p->h);
		pane_resize(p, -1, -1, p->w/2, p->h);
		break;
	}
	tile_adjust(ret);
	tile_adjust(p);
	return ret;
}

static int tile_destroy(struct pane *p)
{
	struct tileinfo *ti = p->data;
	struct pane *prev = NULL, *next = NULL;
	struct pane *t, *remain;
	int pos, prevpos, nextpos;
	int remaining = 0;

	if (ti->direction == Neither)
		/* Cannot destroy root (yet) */
		return 0;

	if (p->parent == NULL) {
		/* subsumed husk being destroyed */
		free(ti);
		return 1;
	}

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
	ASSERT(remaining > 0);
	if (prev == NULL) {
		/* next gets the space and focus*/
		if (ti->direction == Horiz)
			pane_resize(next, p->x, next->y,
				    p->w + next->w, next->h);
		else
			pane_resize(next, next->x, p->y,
				    next->w, p->h + next->h);
		tile_adjust(next);
		p->parent->focus = next;
	} else if (next == NULL) {
		/* prev gets the space and focus */
		if (ti->direction == Horiz)
			pane_resize(prev, -1, -1, prev->w + p->w, prev->h);
		else
			pane_resize(prev, -1, -1, prev->w, prev->h + p->h);
		tile_adjust(prev);
		p->parent->focus = prev;
	} else {
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
			pane_resize(prev, -1, -1, prev->w + w, prev->h);
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
			pane_resize(prev, -1, -1, prev->w, prev->h + h);
			h = p->h - h;
			pane_resize(next, next->x, prev->y + prev->h,
				    next->w , next->h + h);
		}
		tile_adjust(next);
		tile_adjust(prev);
	}
	list_del(&ti->tiles);
	free(ti);
	if (remaining == 1) {
		struct tileinfo *ti2;
		/* Only one child left, must move it into parent.
		 * Cannot destroy the parent, so bring child into parent */
		p = remain->parent;

		ti = remain->data;
		ti2 = p->data;
		ti->direction = ti2->direction;

		pane_subsume(remain, p);
		ti->p = p;
		ti2->p = remain;
		pane_close(remain);
		pane_damaged(p, DAMAGED_SIZE);
	}
	return 1;
}

static void tile_avail(struct pane *p, struct pane *ignore)
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
			if (t == ignore || t->z)
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

static void tile_adjust(struct pane *p)
{
	/* Size of pane 'p' has changed and it is time to adjust
	 * the children, both their size and offset.
	 * For the non-stacking direction, offset must be zero and
	 * size is copied from p.
	 * For the stacking direction, we count used size, find the
	 * change required and distribute that.  This relies on that
	 * fact that 'tail_avail' must have been run and it left
	 * avail space calculations around.
	 */
	struct pane *t;
	int used = 0;
	int cnt = 0;
	int avail_cnt = 0;
	int pos;
	int size;
	struct tileinfo *ti = p->data;

	if (ti->leaf)
		/* Children are responsible for themselves. */
		return;

	list_for_each_entry(t, &p->children, siblings) {

		if (t->z)
			continue;
		ti = t->data;
		if (ti->direction == Horiz) {
			t->y = 0;
			t->h = p->h;
			used += t->w;
			size = p->w;
		} else {
			t->x = 0;
			t->w = p->w;
			used += t->h;
			size = p->h;
		}
		pane_damaged(t, DAMAGED_SIZE);
		if (ti->avail_inline)
			avail_cnt++;
		cnt++;
	}
	while (used != size) {
		int change = 0;

		if (used > size)
			cnt = avail_cnt;
		avail_cnt = 0;
		list_for_each_entry(t, &p->children, siblings) {
			struct tileinfo *ti2 = t->data;
			int diff;
			if (t->z)
				continue;
			if (used > size) {
				/* shrinking */
				if (ti2->avail_inline == 0)
					continue;
				diff = (used - size + (used%cnt)) / cnt;
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
				diff = (size - used + (size%cnt)) / cnt;

			if (diff)
				change = 1;
			if (ti2->direction == Horiz) {
				t->w += diff;
				used += diff;
				cnt--;
			} else {
				t->h += diff;
				used += diff;
				cnt--;
			}
			pane_damaged(t, DAMAGED_SIZE);
		}
		if (!change)
			break;
	}
	pos = 0;
	list_for_each_entry(t, &p->children, siblings) {
		struct tileinfo *ti2 = t->data;
		if (t->z)
			continue;
		if (ti2->direction == Horiz) {
			t->x = pos;
			pos += t->w;
		} else {
			t->y = pos;
			pos += t->h;
		}
		pane_damaged(t, DAMAGED_SIZE);
		tile_adjust(t);
	}
}

static int tile_grow(struct pane *p, int horiz, int size)
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
		return 0;
	if (size < 0) {
		/* Does this pane have room to shrink */
		tile_avail(p, NULL);
		if (ti->direction == (horiz? Horiz : Vert))
			avail = ti->avail_inline;
		else
			avail = ti->avail_perp;
		if (avail < -size)
			return 0;
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
			if (t->z)
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
			return 1;
		if (ti->direction == Horiz) {
			p->w += size;
			other->w -= size;
		} else{
			p->h += size;
			other->h -= size;
		}
		pane_damaged(p, DAMAGED_SIZE);
		tile_adjust(p->parent);
		return 1;
	}

	/* Hoping to grow if there is room for others to shrink */
	tile_avail(p->parent, p);
	tip = p->parent->data;
	if (ti->direction == (horiz ? Horiz : Vert))
		avail= tip->avail_inline;
	else
		avail = tip->avail_perp;
	if (avail < size)
		return 0;
	if (ti->direction == Horiz)
		p->w += size;
	else
		p->h += size;
	pane_damaged(p, DAMAGED_SIZE);
	ti->avail_inline = 0; /* make sure this one doesn't suffer */
	tile_adjust(p->parent);
	return 1;
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
		if ((p2->z != 0) != popup)
			continue;
		return p2;
	}
	return NULL;
}

static struct tileinfo *tile_first(struct tileinfo *ti)
{
	while (!ti->leaf) {
		struct pane *p = next_child(ti->p, NULL, 0);
		if (!p)
			return NULL;
		ti = p->data;
	}
	return ti;
}

static int tile_is_first(struct tileinfo *ti)
{
	while (ti->direction != Neither) {
		if (ti->p != next_child(ti->p->parent, NULL, 0))
			return 0;
		ti = ti->p->parent->data;
	}
	return 1;
}

static struct pane *tile_root_popup(struct tileinfo *ti)
{
	while (ti->direction != Neither)
		ti = ti->p->parent->data;
	return next_child(ti->p, NULL, 1);
}


DEF_CMD(tile_command)
{
	struct pane *p = ci->home;
	struct pane *p2;
	struct tileinfo *ti = p->data;
	struct tileinfo *t2;
	char *cmd = ci->key + 7; /* "Window:" */

	if (strcmp(cmd, "next")==0) {
		/* If currently on a popup, go to next popup if there is one, else
		 * to this tile.
		 * If was not on a pop-up, go to next tile and if there is a popup,
		 * go there.
		 */
		if (p->focus->z) {
			p2 = next_child(p, p->focus, 1);
			if (p2) {
				pane_focus(p2);
				return 1;
			} else if (ti->leaf) {
				pane_focus(ti->content);
				return 1;
			}
			t2 = tile_first(ti);
		} else {
			if (ti->leaf) {
				t2 = list_next_entry(ti, tiles);
				if (tile_is_first(t2) &&
				    (p2 = tile_root_popup(t2)) != NULL) {
					pane_focus(p2);
					return 1;
				}
			} else
				t2 = tile_first(ti);
		}
		pane_focus(t2->p);
		p2 = next_child(t2->p, NULL, 1);
		if (p2)
			pane_focus(p2);
	} else if (strcmp(cmd, "prev")==0) {
		t2 = list_prev_entry(ti, tiles);
		pane_focus(t2->p);
	} else if (strcmp(cmd, "x+")==0) {
		tile_grow(p, 1, RPT_NUM(ci));
		pane_damaged(p, DAMAGED_SIZE);
	} else if (strcmp(cmd, "x-")==0) {
		tile_grow(p, 1, -RPT_NUM(ci));
		pane_damaged(p, DAMAGED_SIZE);
	} else if (strcmp(cmd, "y+")==0) {
		tile_grow(p, 0, RPT_NUM(ci));
		pane_damaged(p, DAMAGED_SIZE);
	} else if (strcmp(cmd, "y-")==0) {
		tile_grow(p, 0, -RPT_NUM(ci));
		pane_damaged(p, DAMAGED_SIZE);
	} else if (strcmp(cmd, "split-x")==0) {
		p2 = tile_split(p, 1, 1);
		pane_clone_children(ci->home, p2);
	} else if (strcmp(cmd, "split-y")==0) {
		p2 = tile_split(p, 0, 1);
		pane_clone_children(ci->home, p2);
	} else if (strcmp(cmd, "close")==0) {
		if (ti->direction != Neither)
			pane_close(p);
	} else if (strcmp(cmd, "close-others") == 0) {
		/* close all other panes in the 'tiles' list. */
		while (!list_empty(&ti->tiles)) {
			struct tileinfo *ti2 = list_next_entry(ti, tiles);
			pane_close(ti2->p);
		}
	} else
		return 0;
	return 1;
}

DEF_CMD(tile_other)
{
	/* Choose some other tile.  If there aren't any, make one.
	 * Result is returned in ci->focus
	 */
	struct pane *p = ci->home;
	struct pane *p2;
	struct tileinfo *ti = p->data;

	if (!ti->leaf)
		return 0;
	if (!list_empty(&ti->tiles)) {
		struct tileinfo *ti2 = list_next_entry(ti, tiles);
		struct pane *c = ti2->content;
		ti2->leaf = 2;
		if (c)
			pane_close(c);
		ti2->leaf = 1;
		return comm_call(ci->comm2, "callback:pane", ti2->p, 0,
				 NULL, NULL, 0);
	}
	/* Need to create a tile.  If wider than 120 (FIXME configurable and
	 * pixel sensitive), horiz-split else vert
	 */
	p2 = tile_split(p, p->w >= 120, 1);
	if (p2)
		return comm_call(ci->comm2, "callback:pane", p2, 0,
				 NULL, NULL, 0);
	return -1;
}

DEF_CMD(tile_this)
{
	struct tileinfo *ti = ci->home->data;
	if (!ti->leaf)
		return 0;
	return comm_call(ci->comm2, "callback:pane", ci->home, 0,
			 NULL, NULL, 0);
}

DEF_CMD(tile_root)
{
	struct pane *p = ci->home;
	struct tileinfo *ti = p->data;

	if (ti->direction != Neither)
		return 0;

	return comm_call(ci->comm2, "callback:pane", p, 0,
			 NULL, NULL, 0);
}

DEF_CMD(tile_child_closed)
{
	struct pane *p = ci->home;
	struct tileinfo *ti = p->data;
	struct pane *c;

	if (ti->leaf != 1)
		return 1;
	if (ci->focus->z != 0)
		return 1;
	/* Child closed, but we were, so find something else to display */
	c = call_pane("docs:choose", p, 0, NULL, 0);
	if (c)
		doc_attach_view(p, c, NULL);
	return 1;
}

DEF_CMD(tile_child_registered)
{
	struct pane *p = ci->home;
	struct tileinfo *ti = p->data;
	struct pane *c = ci->focus;

	if (ti->leaf && c->z == 0) {
		if (ti->content) {
			ti->leaf = 2;
			pane_close(ti->content);
			ti->leaf = 1;
		}
		ti->content = c;
	}
	return 1;
}

void edlib_init(struct pane *ed)
{
	tile_map = key_alloc();

	key_add_range(tile_map, "Window:", "Window;", &tile_command);
	key_add(tile_map, "OtherPane", &tile_other);
	key_add(tile_map, "ThisPane", &tile_this);
	key_add(tile_map, "RootPane", &tile_root);
	key_add(tile_map, "Clone", &tile_clone);
	key_add(tile_map, "Window:scale-relative", &tile_scale);
	key_add(tile_map, "ChildClosed", &tile_child_closed);
	key_add(tile_map, "ChildRegistered", &tile_child_registered);

	call_comm("global-set-command", ed, 0, NULL, "attach-tile",
		  0, &tile_attach);
}
