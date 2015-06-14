/*
 * Keymaps for libed.
 *
 * A keymap maps a key to a command.
 * Keys are ordered for fast binary-search lookup.
 * A "key" includes and modifiers which can be registered separately.
 * 21 bits represent a particular key.  This covers all of Unicode
 * and a bit more.  1FFFxx is used for function keys with numbers
 * aligning with curses KEY_*codes.  1FFExx is used for mouse button.
 * This leaves 11 bits in a u32 for modifiers.
 * e.g. meta shift control C-x C-c etc.
 *
 * A 'command' is a struct provided by any of various
 * modules.
 *
 * Modifiers are global and can be registered.  Doing so returns
 * a command which can be then bound to a key to effect that
 * modifier.
 *
 * A range can be stored by starting first and last, and having
 * a NULL command for the last.
 */

#include <unistd.h>
#include <stdlib.h>
#include <memory.h>

#include "list.h"
#include "pane.h"
#include "keymap.h"

struct map {
	int	size;
	int	*keys;
	struct command **comms;
};

static struct modmap {
	char *name;
	struct command comm;
} modmap[11] = {{0}};

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
static int key_find(struct map *map, int k)
{
	int lo = 0;
	int hi = map->size;

	/* all entries before 'lo' are < k.
	 * all entries at 'hi' or later are >= k.
	 * So when lo==hi, hi is the answer.
	 */
	while (hi > lo) {
		int mid = (hi + lo)/ 2;
		if (map->keys[mid] < k)
			lo = mid+1;
		else
			hi = mid;
	}
	return hi;
}

void key_add(struct map *map, int k, struct command *comm)
{
	int size = size2alloc(map->size + 1);
	int pos;

	if (!comm)
		return;

	pos = key_find(map, k);
	if (pos < map->size && map->keys[pos] == k)
		/* Ignore duplicate */
		return;
	if (pos < map->size && map->comms[pos] == NULL)
		/* in middle of a range */
		return;

	if (size2alloc(map->size) != size) {
		map->keys = realloc(map->keys, size * sizeof(int));
		map->comms = realloc(map->comms,
				     size * sizeof(struct command *));
	}

	memmove(map->keys+pos+1, map->keys+pos,
		(map->size - pos) * sizeof(int));
	memmove(map->comms+pos+1, map->comms+pos,
		(map->size - pos) * sizeof(struct command *));
	map->keys[pos] = k;
	map->comms[pos] = comm;
	map->size += 1;
}

void key_add_range(struct map *map, int first, int last,
		   struct command *comm)
{
	int size = size2alloc(map->size + 2);
	int pos, pos2;

	if (!comm)
		return;

	pos = key_find(map, first);
	if (pos < map->size && map->keys[pos] == first)
		/* Ignore duplicate */
		return;
	if (pos < map->size && map->comms[pos] == NULL)
		/* in middle of a range */
		return;

	pos2 = key_find(map, last);
	if (pos != pos2)
		/* Overlaps existing keys */
		return;

	if (size2alloc(map->size) != size) {
		map->keys = realloc(map->keys, size * sizeof(int));
		map->comms = realloc(map->comms,
				     size * sizeof(struct command *));
	}

	memmove(map->keys+pos+2, map->keys+pos,
		(map->size - pos) * sizeof(int));
	memmove(map->comms+pos+2, map->comms+pos,
		(map->size - pos) * sizeof(struct command *));
	map->keys[pos] = first;
	map->comms[pos] = comm;
	map->keys[pos+1] = last;
	map->comms[pos+1] = NULL;
	map->size += 2;
}

void key_del(struct map *map, int k)
{
	int pos;

	pos = key_find(map, k);
	if (pos >= map->size || map->keys[pos] != k)
		return;

	memmove(map->keys+pos, map->keys+pos+1,
		(map->size-pos-1) * sizeof(int));
	memmove(map->comms+pos, map->comms+pos+1,
		(map->size-pos-1) * sizeof(struct command *));
	map->size -= 1;
}

static int key_modify(struct command *comm, int key, struct cmd_info *ci)
{
	struct modmap *m = container_of(comm, struct modmap, comm);
	int i = m - modmap;

	pane_set_modifier(ci->focus, 1<<(i+21));
	return 1;
}

struct command *key_register_mod(char *name, int *bit)
{
	int i;
	int free = -1;
	for (i = 0; i < 11; i++) {
		if (!modmap[i].name) {
			if (free < 0)
				free = i;
			continue;
		}
		if (strcmp(modmap[i].name, name) == 0) {
			*bit = 1 << (i + 21);
			return &modmap[i].comm;
		}
	}
	if (free < 0)
		return NULL;
	modmap[free].name = strdup(name);
	modmap[free].comm.func = key_modify;
	modmap[free].comm.name = name;
	*bit  = 1 << (free+21);
	return &modmap[free].comm;
}

int key_lookup(struct map *m, int key, struct cmd_info *ci)
{
	int pos = key_find(m, key);
	struct command *comm;

	if (pos >= m->size)
		return 0;
	if (m->comms[pos] == NULL &&
	    pos > 0)
		comm = m->comms[pos-1];
	else if (m->keys[pos] == key)
		comm = m->comms[pos];
	else
		return 0;
	return comm->func(comm, key, ci);
}
