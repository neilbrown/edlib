/*
 * Tile manager for edlib.
 * This could be implemented as a plug-in eventually (I think).
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
#include <curses.h>
#include <string.h>

#include "core.h"
#include "pane.h"
#include "view.h"

#include "extras.h"

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
	int				avail_inline;
	int				avail_perp;
	struct list_head		tiles; /* headless ordered list of all tiles
						* in the tree.  Used for next/prev
						*/
	struct pane			*p;
};

static struct map *tile_map;
static void tile_adjust(struct pane *p);
static void tile_avail(struct pane *p, struct pane *ignore);
static int tile_destroy(struct pane *p);

static int do_tile_refresh(struct command *c, struct cmd_info *ci)
{
	struct pane *p = ci->focus;
	int damage = ci->extra;
	struct tileinfo *ti = p->data;

	if (strcmp(ci->key, "Close") == 0) {
		tile_destroy(p);
		return 0;
	}

	if (strcmp(ci->key, "Refresh") != 0)
		return 0;

	if (p->focus == NULL && !list_empty(&p->children))
		p->focus = list_first_entry(&p->children, struct pane, siblings);

	if ((damage & DAMAGED_SIZE) && ti->direction == Neither) {
		pane_resize(p, 0, 0, p->parent->w, p->parent->h);
		tile_avail(p, NULL);
		tile_adjust(p);
	}
	return 0;
}
DEF_CMD(tile_refresh, do_tile_refresh, "tile-refresh");

struct pane *tile_init(struct pane *display)
{
	struct tileinfo *ti = malloc(sizeof(*ti));
	struct pane *p = pane_register(display, 0, &tile_refresh, ti, NULL);
	p->keymap = tile_map;
	ti->p = p;
	ti->direction = Neither;
	INIT_LIST_HEAD(&ti->tiles);
	pane_resize(p, 0, 0, display->w, display->h);
	return p;
}

struct pane *tile_split(struct pane *p, int horiz, int after)
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
		space = p->h;
	else
		space = p->w;

	/* FIXME ask the leafs */
	if (space < 8)
		return NULL;

	if (ti->direction != (horiz? Horiz : Vert)) {
		/* This tile is not split in the required direction, need
		 * to create an extra level.
		 */
		struct pane *p2;
		ti2 = malloc(sizeof(*ti2));
		ti2->direction = ti->direction;
		INIT_LIST_HEAD(&ti2->tiles);
		p2 = pane_register(p->parent, 0, &tile_refresh, ti2, &p->siblings);
		p2->keymap = tile_map;
		ti2->p = p2;
		pane_resize(p2, p->x, p->y, p->w, p->h);
		pane_reparent(p, p2, NULL);
		pane_resize(p, 0, 0, 0, 0);
		ti->direction = horiz ? Horiz : Vert;
	}
	here = after ? &p->siblings : p->siblings.prev;
	ti2 = malloc(sizeof(*ti2));
	ti2->direction = ti->direction;
	list_add(&ti2->tiles, &ti->tiles);
	ret = pane_register(p->parent, 0, &tile_refresh, ti2, here);
	ret->keymap = tile_map;
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

	if (ti->direction == Vert)
		pos = p->y;
	else
		pos = p->x;
	prevpos = nextpos = -1;
	list_for_each_entry(t, &p->parent->children, siblings) {
		int pos2;
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
		/* next gets the space */
		if (ti->direction == Horiz)
			pane_resize(next, p->x, next->y,
				    p->w + next->w, next->h);
		else
			pane_resize(next, next->x, p->y,
				    next->w, p->h + next->h);
		tile_adjust(next);
	} else if (next == NULL) {
		/* prev gets the space */
		if (ti->direction == Horiz)
			pane_resize(prev, -1, -1, prev->w + p->w, prev->h);
		else
			pane_resize(prev, -1, -1, prev->w, prev->h + p->h);
		tile_adjust(prev);
	} else {
		/* share the space */
		if (ti->direction == Horiz) {
			int w = p->w / 2;
			pane_resize(prev, -1, -1, prev->w + w, prev->h);
			w = p->w - w;
			pane_resize(next, prev->x + prev->w, next->y,
				    next->w + w, next->h);
		} else {
			int h = p->h / 2;
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
		pane_free(remain);
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

#if 0
	if (ti->direction == Neither) {
		t = list_first_entry(&p->children, struct pane, siblings);
		if (t->refresh == &tile_refresh) {
			p = t;
			ti = p->data;
		}
	}
#endif

	if (p->children.next == p->children.prev) {
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
			if (t == ignore)
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
	if (list_empty(&p->children))
		return;
	if (p->children.next == p->children.prev) {
		t = list_first_entry(&p->children, struct pane, siblings);
		pane_resize(t, 0, 0, p->w, p->h);
		return;
	}
	list_for_each_entry(t, &p->children, siblings) {
		struct tileinfo *ti = t->data;
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
			struct tileinfo *ti = t->data;
			int diff;
			if (used > size) {
				/* shrinking */
				if (ti->avail_inline == 0)
					continue;
				diff = (used - size + (used%cnt)) / cnt;
				if (diff > ti->avail_inline)
					diff = ti->avail_inline;
				ti->avail_inline -= diff;
				if (ti->avail_inline)
					/* Still space available if needed */
					avail_cnt++;

				diff = -diff;
			} else if (used == size)
				break;
			else
				diff = (size - used + (size%cnt)) / cnt;

			if (diff)
				change = 1;
			if (ti->direction == Horiz) {
				t->w += diff;
				used += diff;
				cnt--;
			} else {
				t->h += diff;
				used += diff;
				cnt--;
			}
		}
		if (!change)
			break;
	}
	pos = 0;
	list_for_each_entry(t, &p->children, siblings) {
		struct tileinfo *ti = t->data;
		if (ti->direction == Horiz) {
			t->x = pos;
			pos += t->w;
		} else {
			t->y = pos;
			pos += t->h;
		}
		tile_adjust(t);
	}
}

int tile_grow(struct pane *p, int horiz, int size)
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
		struct pane *other;
		if (p->siblings.prev != &p->parent->children)
			other = list_prev_entry(p, siblings);
		else
			other = list_next_entry(p, siblings);
		if (ti->direction == Horiz) {
			p->w += size;
			other->w -= size;
		} else{
			p->h += size;
			other->h -= size;
		}
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
	ti->avail_inline = 0; /* make sure this one doesn't suffer */
	tile_adjust(p->parent);
	return 1;
}

static int tile_command(struct command *c, struct cmd_info *ci)
{
	struct pane *p = ci->focus;
	struct pane *p2;
	struct tileinfo *ti = p->data;
	struct tileinfo *t2;

	if (!ci->str)
		return 0;
	if (strcmp(ci->str, "next")==0) {
		t2 = list_next_entry(ti, tiles);
		pane_focus(t2->p);
	} else if (strcmp(ci->str, "prev")==0) {
		t2 = list_prev_entry(ti, tiles);
		pane_focus(t2->p);
	} else if (strcmp(ci->str, "x+")==0) {
		tile_grow(p, 1, RPT_NUM(ci));
		pane_damaged(p, DAMAGED_SIZE);
	} else if (strcmp(ci->str, "x-")==0) {
		tile_grow(p, 1, -RPT_NUM(ci));
		pane_damaged(p, DAMAGED_SIZE);
	} else if (strcmp(ci->str, "y+")==0) {
		tile_grow(p, 0, RPT_NUM(ci));
		pane_damaged(p, DAMAGED_SIZE);
	} else if (strcmp(ci->str, "y-")==0) {
		tile_grow(p, 0, -RPT_NUM(ci));
		pane_damaged(p, DAMAGED_SIZE);
	} else if (strcmp(ci->str, "split-x")==0) {
		p2 = tile_split(p, 1, 1);
		render_text_attach(view_attach(p2, ci->point_pane->point->doc, 1),
				   ci->point_pane->point);
	} else if (strcmp(ci->str, "split-y")==0) {
		p2 = tile_split(p, 0, 1);
		render_text_attach(view_attach(p2, ci->point_pane->point->doc, 1),
				   ci->point_pane->point);
	} else if (strcmp(ci->str, "close")==0) {
		if (ti->direction != Neither)
			pane_close(p);
	} else
		return 0;
	return 1;
}
DEF_CMD(comm_tile, tile_command, "tile-command");

void tile_register(struct map *m)
{
	tile_map = key_alloc();

	key_add(tile_map, "WindowOP", &comm_tile);
}
