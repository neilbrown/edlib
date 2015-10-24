/*
 * Keymaps for libed.
 *
 * A keymap maps a key to a command.
 * Keys are ordered for fast binary-search lookup.
 * A "key" includes a mode which can be registered separately, and
 * two modifier bits: alt/meta and super.
 * 21 bits represent a particular key.  This covers all of Unicode
 * and a bit more.  1FFFxx is used for function keys with numbers
 * aligning with curses KEY_*codes.  1FFExx is used for mouse button.
 * 1 bit is used for 'META' aka 'ALT'.  1 for 'super'.
 * Shift and Ctrl are included in the key itself in different ways.
 * Remaining 9 bits identify a mode or modifier such as 'emacs' or 'vi'
 * or 'C-x' or 'C-c' or 'VI-insert' etc
 *
 * A 'command' is a struct provided by any of various
 * modules.
 *
 * Modes are global and can be registered.  Doing so returns
 * a command which can be then bound to a key to effect that
 * mode.  Modifies are either transient or stable.  Stable
 * modifiers must be explicitly 'replaced'.
 *
 * A range can be stored by stating first and last, and having
 * a NULL command for the last.
 */

#include <unistd.h>
#include <stdlib.h>
#include <memory.h>

#include "core.h"
#include "pane.h"

struct map {
	int	size;
	char	**keys;
	struct command **comms;
};

static struct modmap {
	char	*name;
	bool	transient;
	struct command comm;
} modmap[512] = {{0}};

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

int key_add(struct map *map, char *k, struct command *comm)
{
	int size = size2alloc(map->size + 1);
	int pos;

	if (!comm)
		return 0;

	pos = key_find(map, k);
	if (pos < map->size && strcmp(map->keys[pos], k) == 0)
		/* Ignore duplicate */
		return 0;
	if (pos < map->size && map->comms[pos] == NULL)
		/* in middle of a range */
		return 0;

	if (size2alloc(map->size) != size) {
		map->keys = realloc(map->keys, size * sizeof(map->keys[0]));
		map->comms = realloc(map->comms,
				     size * sizeof(struct command *));
	}

	memmove(map->keys+pos+1, map->keys+pos,
		(map->size - pos) * sizeof(map->keys[0]));
	memmove(map->comms+pos+1, map->comms+pos,
		(map->size - pos) * sizeof(struct command *));
	map->keys[pos] = k;
	map->comms[pos] = comm;
	map->size += 1;
	return 1;
}

int key_add_range(struct map *map, char *first, char *last,
		   struct command *comm)
{
	int size = size2alloc(map->size + 2);
	int pos, pos2;

	if (!comm)
		return 0;

	pos = key_find(map, first);
	if (pos < map->size && strcmp(map->keys[pos], first) == 0)
		/* Ignore duplicate */
		return 0;
	if (pos < map->size && map->comms[pos] == NULL)
		/* in middle of a range */
		return 0;

	pos2 = key_find(map, last);
	if (pos != pos2)
		/* Overlaps existing keys */
		return 0;

	if (size2alloc(map->size) != size) {
		map->keys = realloc(map->keys, size * sizeof(map->keys[0]));
		map->comms = realloc(map->comms,
				     size * sizeof(struct command *));
	}

	memmove(map->keys+pos+2, map->keys+pos,
		(map->size - pos) * sizeof(map->keys[0]));
	memmove(map->comms+pos+2, map->comms+pos,
		(map->size - pos) * sizeof(struct command *));
	map->keys[pos] = first;
	map->comms[pos] = comm;
	map->keys[pos+1] = last;
	map->comms[pos+1] = NULL;
	map->size += 2;
	return 1;
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

static int key_mode(struct command *comm, struct cmd_info *ci)
{
	struct modmap *m = container_of(comm, struct modmap, comm);

	pane_set_mode(ci->focus, m->name, m->transient);
	return 1;
}

struct command *key_register_mode(char *name)
{
	int i;
	int free = 0;
	for (i = 1; i < 512; i++) {
		if (!modmap[i].name) {
			if (!free)
				free = i;
			continue;
		}
		if (strcmp(modmap[i].name, name) == 0)
			return &modmap[i].comm;
	}
	if (!free)
		return NULL;
	modmap[free].name = strdup(name);
	modmap[free].transient = 1;
	modmap[free].comm.func = key_mode;
	modmap[free].comm.name = name;
	return &modmap[free].comm;
}

static int key_lookup(struct map *m, struct cmd_info *ci)
{
	int pos = key_find(m, ci->key);
	struct command *comm;
	struct cmd_info ci2 = *ci;

	if (pos >= m->size)
		return 0;
	if (m->comms[pos] == NULL &&
	    pos > 0)
		comm = m->comms[pos-1];
	else if (strcmp(m->keys[pos], ci2.key) == 0)
		comm = m->comms[pos];
	else
		return 0;
	return comm->func(comm, &ci2);
}

int key_handle(struct cmd_info *ci)
{
	struct pane *p = ci->focus;
	int ret = 0;

	while (ret == 0 && p) {
		if (p->keymap) {
			ci->focus = p;
			ret = key_lookup(p->keymap, ci);
		}
		if (ci->x >= 0) {
			ci->x += p->x;
			ci->y += p->y;
		}
		p = p->parent;
	}
	return ret;
}

int key_handle_focus(struct cmd_info *ci)
{
	/* Handle this in the focus pane, so x,y are irrelevant */
	ci->x = -1;
	ci->y = -1;
	if (ci->focus->point)
		ci->point_pane = ci->focus;
	while (ci->focus->focus) {
		ci->focus = ci->focus->focus;
		if (ci->focus->point)
			ci->point_pane = ci->focus;
	}
	return key_handle(ci);
}

int key_handle_xy(struct cmd_info *ci)
{
	/* Handle this in child with x,y co-ords */
	struct pane *p = ci->focus;
	int x = ci->x;
	int y = ci->y;

	if (p->point)
		ci->point_pane = p;
	while (1) {
		struct pane *t, *chld = NULL;

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
		if (p->point)
			ci->point_pane = p;
	}
	ci->x = x;
	ci->y = y;
	ci->focus = p;
	return key_handle(ci);
}
