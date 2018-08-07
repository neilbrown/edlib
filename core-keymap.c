/*
 * Copyright Neil Brown ©2015 <neil@brown.name>
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

inline static int qhash(char key, unsigned int start)
{
	return (start ^ key) * 0x61C88647U;
}

struct map {
	unsigned long bloom[256 / (sizeof(long)*8) ];
	short	changed;
	short	prefix_len;
	int	size;
	struct map *chain;
	char	* safe *keys safe;
	struct command * safe *comms safe;
};

static inline struct command * safe GETCOMM(struct command *c safe)
{
	return (struct command * safe)(((unsigned long)c) & ~1UL);
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

struct map *safe key_alloc(void)
{
	struct map *m = calloc(1, sizeof(*m));

	m->prefix_len = -1;
	return m;
}

void key_free(struct map *m safe)
{
	int i;
	for (i = 0; i < m->size; i++) {
		free(m->keys[i]);
		command_put(GETCOMM(m->comms[i]));
	}
	free(m->keys);
	free(m->comms);
	free(m);
}

static int hash_str(char *key safe, int len)
{
	int i;
	int h = 0;
	for (i = 0; (len < 0 || i < len) && key[i]; i++)
		h = qhash(key[i], h);
	return h;
}

inline static void set_bit(unsigned long *set safe, int bit)
{
	set[bit/(sizeof(unsigned long)*8)] |= 1UL << (bit % (sizeof(unsigned long)*8));
}

inline static int test_bit(unsigned long *set safe, int bit)
{
	return !! (set[bit/(sizeof(unsigned long)*8)] & (1UL << (bit % (sizeof(unsigned long)*8))));
}


static int key_present(struct map *map safe, char *key, int klen, unsigned int *hashp safe)
{
	int hash;

	if (map->changed) {
		int i;
		for (i = 0; i < map->size; i++) {
			hash = hash_str(map->keys[i], map->prefix_len);
			set_bit(map->bloom, hash&0xff);
			set_bit(map->bloom, (hash>>8)&0xff);
			set_bit(map->bloom, (hash>>16)&0xff);
		}
		map->changed = 0;
	}
	if (map->prefix_len < 0 || klen <= map->prefix_len)
		hash = hashp[0];
	else
		hash = hashp[map->prefix_len];
	return (test_bit(map->bloom, hash&0xff) &&
		test_bit(map->bloom, (hash>>8)&0xff) &&
		test_bit(map->bloom, (hash>>16)&0xff));
}

/* Find first entry >= k */
static int key_find(struct map *map safe, char *k safe)
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

void key_add(struct map *map safe, char *k safe, struct command *comm)
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
			/* Changing the start of a range, insert an exact match */
		} else {
			/* replace a non-range */
			command_put(map->comms[pos]);
			map->comms[pos] = command_get(comm);
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
	map->keys[pos] = strdup(k);
	map->comms[pos] = command_get(comm);
	if (comm2) {
		map->keys[pos+1] = strdup(k);
		map->comms[pos+1] = SET_RANGE(command_get(GETCOMM(comm2)));
	}
	map->size += ins_cnt;
	map->changed = 1;
}

void key_add_range(struct map *map safe, char *first safe, char *last safe,
		   struct command *comm)
{
	int size, move_size;
	int pos, pos2;
	int prefix;
	int i;

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
	for (i = pos + 1; i < pos2; i++) {
		free(map->keys[i]);
		command_put(GETCOMM(map->comms[i]));
	}
	memmove(map->keys+pos2 + move_size, map->keys+pos2,
		(map->size - pos2) * sizeof(map->keys[0]));
	memmove(map->comms+pos2+ move_size, map->comms+pos2,
		(map->size - pos2) * sizeof(struct command *));

	map->comms[pos] = SET_RANGE(comm);
	map->keys[pos+1] = strdup(last);
	map->comms[pos+1] = command_get(comm);
	map->size += move_size;
	map->changed = 1;
	for (prefix = 0;
	     first[prefix] && first[prefix+1] == last[prefix+1];
	     prefix += 1)
		;
	if (map->prefix_len < 0 || map->prefix_len > prefix)
		map->prefix_len = prefix;
}

void key_add_chain(struct map *map safe, struct map *chain)
{
	while (map->chain)
		map = map->chain;
	map->chain = chain;
}


#if 0
void key_del(struct map *map, wint_t k)
{
	int pos;

	pos = key_find(map, k, -1);
	if (pos >= map->size || strcmp(map->keys[pos], k) == 0)
		return;

	memmove(map->keys+pos, map->keys+pos+1,
		(map->size-pos-1) * sizeof(map->keys[0]));
	memmove(map->comms+pos, map->comms+pos+1,
		(map->size-pos-1) * sizeof(struct command *));
	map->size -= 1;
	map->changed = 1;
}
#endif

struct modmap {
	char	*name;
	struct command comm;
};

static int key_prefix(const struct cmd_info *ci safe)
{
	struct modmap *m = container_of(ci->comm, struct modmap, comm);

	call("Mode:set-mode", ci->focus, 0, NULL, m->name);
	call("Mode:set-num", ci->focus, ci->num);
	call("Mode:set-num2", ci->focus, ci->num2);
	return 1;
}

struct command *key_register_prefix(char *name safe)
{
	struct modmap *mm = malloc(sizeof(*mm));

	/* FIXME refcount these */
	mm->name = strdup(name);
	mm->comm.func = key_prefix;
	mm->comm.refcnt = 0;
	mm->comm.free = NULL;
	return &mm->comm;
}

struct command *key_lookup_cmd(struct map *m safe, char *c safe)
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

int key_lookup(struct map *m safe, const struct cmd_info *ci safe)
{
	int pos;
	struct command *comm;

	if (ci->hash && !key_present(m, ci->key, strlen(ci->key), ci->hash))
		return 0;

	pos = key_find(m, ci->key);
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
	((struct cmd_info*)ci)->comm = comm;
	return comm->func(ci);
}

int key_lookup_prefix(struct map *m safe, const struct cmd_info *ci safe)
{
	int pos = key_find(m, ci->key);
	struct command *comm, *prev = NULL;
	int len = strlen(ci->key);
	char *k = ci->key;

	while (pos < m->size && strncmp(m->keys[pos], k, len) == 0) {
		comm = GETCOMM(m->comms[pos]);
		if (comm && comm != prev) {
			int ret;
			((struct cmd_info*)ci)->comm = comm;
			((struct cmd_info*)ci)->key = m->keys[pos];
			ret = comm->func(ci);
			ASSERT(ret >= 0 || ret < Eunused);
			if (ret)
				return ret;
			prev = comm;
		}
		pos += 1;
	}
	((struct cmd_info*)ci)->key = k;
	return 0;
}

int key_lookup_cmd_func(const struct cmd_info *ci safe)
{
	struct lookup_cmd *l = container_of(ci->comm, struct lookup_cmd, c);
	struct map *m = safe_cast *l->m;
	int ret = key_lookup(m, ci);

	while (!ret && m->chain) {
		m = m->chain;
		ret = key_lookup(m, ci);
	}
	return ret;
}

/* key_handle.  Search towards root for the pane which handles the command.
 */
int key_handle(const struct cmd_info *ci safe)
{
	struct cmd_info *vci = (struct cmd_info*)ci;
	struct pane *p;
	unsigned int hash[30];
	int h= 0;
	int i;

	if ((void*) ci->comm)
		return ci->comm->func(ci);

	for (i = 0; i < 30 && ci->key[i]; i++) {
		h = qhash(ci->key[i], h);
		if (i+1 < 30)
			hash[i+1] = h;
	}
	hash[0] = h;
	if (i < 30)
		vci->hash = hash;

	/* If 'home' is set, search from there, else search
	 * from focus
	 */
	p = ci->home;
	if (!p)
		p = ci->focus;

	while (p) {
		int ret = 0;
		if (p->handle) {
			vci->home = p;
			vci->comm = p->handle;
			ret = p->handle->func(ci);
		}
		if (ret)
			/* 'p' might have been destroyed */
			return ret;
		p = p->parent;
	}
	return 0;
}
