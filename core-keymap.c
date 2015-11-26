/*
 * Copyright Neil Brown <neil@brown.name> 2015
 * May be distributed under terms of GPLv2 - see file:COPYING
 *
 * Keymaps for edlib.
 *
 * A keymap maps a key to a command.
 * Keys are ordered for fast binary-search lookup.
 * A "key" is an arbitrary string which typically contains
 * some 'mode' pr efix and some specific tail.
 * e.g emacs-M-C-Chr-x is Meta-Control-X in emacs mode.
 * As far as the map is concerned, it is just a lexically order string.
 *
 * A 'command' is a struct provided by any of various
 * modules.
 *
 * A range can be stored by setting the lsb of the command pointer at
 * the start of the range.
 * When searching for a key we find the first entry that is not less than the target.
 * If it is an exact match, use it.
 * If previous entry exists and has the lsb set, then use that command.
 *
 * So to add a range, the start is entered with lsb set, and the end it entered with
 * lsb clear.
 *
 * If a key is registered a second time, the new over-rides the old.
 * This is particularly useful for registering a range, and then some exception.
 * To delete a key from a range we need to make two ranges, one that ends
 * just before the new key, one that starts just after.
 * The 'ends just before' is easy - we just add the new key or range.
 * The 'starts just after' is managed by entering the same key twice.
 * The first instance of the key has a 'lsb clear' command and is used for
 * exact matches.  The second instance has 'lsb set' and is used for everything
 * after.
 *
 * A 'prefix' can be registered which creates a command which temporarily
 * enabled the given prefix.  It is applied to the next command, but is
 * discarded after that.  This is just a convenience function.
 *
 */

#include <unistd.h>
#include <stdlib.h>
#include <memory.h>

#include "core.h"

struct map {
	int	size;
	char	**keys;
	struct command **comms;
};

static inline struct command *GETCOMM(struct command *c)
{
	return (struct command *)(((unsigned long)c) & ~1UL);
}

static inline int IS_RANGE(struct command *c)
{
	return ((unsigned long)c) & 1;
}

static inline struct command *SET_RANGE(struct command *c)
{
	return (struct command *)(((unsigned long)c) | 1UL);
}

static int size2alloc(int size)
{
	/* Alway multiple of 8. */
	return ((size-1) | 7) + 1;
}

struct map *key_alloc(void)
{
	struct map *m = malloc(sizeof(*m));
	memset(m, 0, sizeof(*m));
	return m;
}

void key_free(struct map *m)
{
	free(m->keys);
	free(m->comms);
	free(m);
}

/* Find first entry >= k */
static int key_find(struct map *map, char *k)
{
	int lo = 0;
	int hi = map->size;

	/* all entries before 'lo' are < k.
	 * all entries at 'hi' or later are >= k.
	 * So when lo==hi, hi is the answer.
	 */
	while (hi > lo) {
		int mid = (hi + lo)/ 2;
		if (strcmp(map->keys[mid], k) < 0)
			lo = mid+1;
		else
			hi = mid;
	}
	return hi;
}

void key_add(struct map *map, char *k, struct command *comm)
{
	int size;
	int pos;
	struct command *comm2 = NULL;
	int ins_cnt;

	if (!comm)
		return;

	pos = key_find(map, k);
	/* cases:
	 * 1/ match start of range: insert before
	 * 2/ match non-range start: replace
	 * 3/ not in range: insert before like 1
	 * 4/ in a range: insert match and range start.
	 */
	if (pos >= map->size) {
		/* Insert k,comm - default action */
	} else if (strcmp(k, map->keys[pos]) == 0) {
		/* match: need to check if range-start */
		if (IS_RANGE(map->comms[pos])) {
			/* Changing the start of a range, insert and exact match */
		} else {
			/* replace a non-range */
			/* FIXME do I need to release the old command */
			map->comms[pos] = comm;
			return;
		}
	} else if (pos > 0 && IS_RANGE(map->comms[pos-1])) {
		/* insert within a range.
		 * Add given command as non-range match, and old command
		 * as new range start
		 */
		comm2 = map->comms[pos-1];
	} else {
		/* Not in a range, simple insert */
	}

	ins_cnt = comm2 ? 2 : 1;
	size = size2alloc(map->size + ins_cnt);


	if (size2alloc(map->size) != size) {
		map->keys = realloc(map->keys, size * sizeof(map->keys[0]));
		map->comms = realloc(map->comms,
				     size * sizeof(struct command *));
	}

	memmove(map->keys+pos+ins_cnt, map->keys+pos,
		(map->size - pos) * sizeof(map->keys[0]));
	memmove(map->comms+pos+ins_cnt, map->comms+pos,
		(map->size - pos) * sizeof(struct command *));
	map->keys[pos] = k;
	map->comms[pos] = comm;
	if (comm2) {
		map->keys[pos+1] = k;
		map->comms[pos+1] = comm2;
	}
	map->size += ins_cnt;
}

void key_add_range(struct map *map, char *first, char *last,
		   struct command *comm)
{
	int size, move_size;
	int pos, pos2;

	if (!comm || strcmp(first, last) >= 0)
		return;

	/* Add the first entry using key_add */
	key_add(map, first, comm);
	pos = key_find(map, first);
	pos2 = key_find(map, last);

	/* Now 'pos' is a stand-alone entry for 'first'.
	 * If the entry before pos2 is a range start, update to start at 'last',
	 * else discard it, and discard everything else between pos and pos2.
	 * Then insert a stand-alone for 'last' and update 'pos' to be a range-start.
	 */
	if (pos2 - 1 > pos && IS_RANGE(map->comms[pos2-1])) {
		map->keys[pos2-1] = last;
		pos2 -= 1;
	}
	/* Need to insert 'last', and remove extras. so +1 and -(pos2-pos-1); */
	move_size = 1 - (pos2 - pos - 1);
	size = size2alloc(map->size + move_size);
	if (size2alloc(map->size) < size) {
		map->keys = realloc(map->keys, size * sizeof(map->keys[0]));
		map->comms = realloc(map->comms,
				     size * sizeof(struct command *));
	}

	memmove(map->keys+pos2 + move_size, map->keys+pos2,
		(map->size - pos2) * sizeof(map->keys[0]));
	memmove(map->comms+pos2+ move_size, map->comms+pos2,
		(map->size - pos2) * sizeof(struct command *));

	map->comms[pos] = SET_RANGE(comm);
	map->keys[pos+1] = last;
	map->comms[pos+1] = comm;
	map->size += move_size;
	return;
}

#if 0
void key_del(struct map *map, wint_t k)
{
	int pos;

	pos = key_find(map, k);
	if (pos >= map->size || strcmp(map->keys[pos], k) == 0)
		return;

	memmove(map->keys+pos, map->keys+pos+1,
		(map->size-pos-1) * sizeof(map->keys[0]));
	memmove(map->comms+pos, map->comms+pos+1,
		(map->size-pos-1) * sizeof(struct command *));
	map->size -= 1;
}
#endif

struct modmap {
	char	*name;
	bool	transient;
	struct command comm;
};

static int key_prefix(struct cmd_info *ci)
{
	struct modmap *m = container_of(ci->comm, struct modmap, comm);

	pane_set_mode(ci->home, m->name, m->transient);
	return 1;
}

struct command *key_register_prefix(char *name)
{
	struct modmap *mm = malloc(sizeof(*mm));

	mm->name = strdup(name);
	mm->transient = 1;
	mm->comm.func = key_prefix;
	return &mm->comm;
}

struct command *key_lookup_cmd(struct map *m, char *c)
{
	int pos = key_find(m, c);

	if (pos >= m->size)
		return NULL;
	if (strcmp(m->keys[pos], c) == 0) {
		/* Exact match - use this entry */
		return GETCOMM(m->comms[pos]);
	} else if (pos > 0 && IS_RANGE(m->comms[pos-1])) {
		/* In a range, use previous */
		return GETCOMM(m->comms[pos-1]);
	} else
		return NULL;
}

int key_lookup(struct map *m, struct cmd_info *ci)
{
	int pos = key_find(m, ci->key);
	struct command *comm;

	if (pos >= m->size)
		return 0;
	if (strcmp(m->keys[pos], ci->key) == 0) {
		/* Exact match - use this entry */
		comm = GETCOMM(m->comms[pos]);
	} else if (pos > 0 && IS_RANGE(m->comms[pos-1])) {
		/* In a range, use previous */
		comm = GETCOMM(m->comms[pos-1]);
	} else
		return 0;
	ci->comm = comm;
	return comm->func(ci);
}

int key_lookup_cmd_func(struct cmd_info *ci)
{
	struct lookup_cmd *l = container_of(ci->comm, struct lookup_cmd, c);
	return key_lookup(*l->m, ci);
}

int key_handle(struct cmd_info *ci)
{
	struct pane *p = ci->focus;
	int ret = 0;

	if (ci->comm)
		return ci->comm->func(ci);

	ci->hx = ci->x;
	ci->hy = ci->y;
	if (!ci->pointp) {
		struct pane *p2 = p;
		while (p2 && !p2->point)
			p2 = p2->parent;
		if (p2)
			ci->pointp = &p2->point;
	}
	while (ret == 0 && p) {
		if (p->handle) {
			ci->home = p;
			ci->comm = p->handle;
			ret = p->handle->func(ci);
		}
		if (ret)
			/* 'p' might have been destroyed */
			break;
		if (ci->hx >= 0) {
			ci->hx += p->x;
			ci->hy += p->y;
		}
		p = p->parent;
	}
	return ret;
}

int key_handle_focus(struct cmd_info *ci)
{
	/* Handle this in the focus pane, so x,y are irrelevant */
	struct pane *p = ci->focus;
	ci->x = -1;
	ci->y = -1;
	while (p->focus) {
		if (p->point && !ci->pointp)
			ci->pointp = &p->point;
		p = p->focus;
	}
	ci->focus = p;
	ci->comm = NULL;
	return key_handle(ci);
}

int key_handle_xy(struct cmd_info *ci)
{
	/* Handle this in child with x,y co-ords */
	struct pane *p = ci->focus;
	int x = ci->x;
	int y = ci->y;

	while (1) {
		struct pane *t, *chld = NULL;

		if (p->point)
			ci->pointp = &p->point;

		list_for_each_entry(t, &p->children, siblings) {
			if (x < t->x || x >= t->x + t->w)
				continue;
			if (y < t->y || y >= t->y + t->h)
				continue;
			if (chld == NULL || t->z > chld->z)
				chld = t;
		}
		/* descend into chld */
		if (!chld)
			break;
		x -= chld->x;
		y -= chld->y;
		p = chld;
	}
	ci->x = x;
	ci->y = y;
	ci->focus = p;
	ci->comm = NULL;
	return key_handle(ci);
}
