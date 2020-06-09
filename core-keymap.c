/*
 * Copyright Neil Brown ©2015-2020 <neil@brown.name>
 * May be distributed under terms of GPLv2 - see file:COPYING
 *
 * Keymaps for edlib.
 *
 * A keymap maps a key to a command.
 * Keys are ordered for fast binary-search lookup.
 * A "key" is an arbitrary string which typically contains
 * some 'mode' prefix and some specific tail.
 * e.g emacs:M:C-x is Meta-Control-X in emacs mode.
 * As far as the map is concerned, it is just a lexically order string.
 *
 * A 'command' is a struct provided by any of various
 * modules.
 *
 * A range can be stored by setting the lsb of the command pointer at
 * the start of the range.
 * When searching for a key we find the first entry that is not less
 * than the target.  If it is an exact match, use it.  If previous entry
 * exists and has the lsb set, then use that command.
 *
 * So to add a range, the start is entered with lsb set, and the end it
 * entered with lsb clear.
 *
 * If a key is registered a second time, the new over-rides the old.
 * This is particularly useful for registering a range, and then some
 * exceptions.
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
#include "misc.h"

inline static int qhash(char key, unsigned int start)
{
	return (start ^ key) * 0x61C88647U;
}

struct map {
	unsigned long	bloom[256 / (sizeof(unsigned long)*8) ];
	short		changed;
	short		size;
	struct map	*chain;
	char		* safe *keys safe;
	struct command	* safe *comms safe;
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

DEF_CMD(keymap_list)
{
	struct map *m;
	int len = ci->str ? strlen(ci->str) : 0;
	int i;

	if (ci->comm == &keymap_list)
		/* should be impossible */
		return 0;
	/* ci->comm MUST be the keymap */
	m = (struct map* safe)ci->comm;

	for (i = 0; i < m->size; i++)
		if (!len || strncmp(ci->str, m->keys[i], len) == 0)
			if (comm_call(ci->comm2, "cb", ci->focus,
				      IS_RANGE(m->comms[i]), NULL, m->keys[i]) <= 0)
				break;
	return Efallthrough;
}

static int size2alloc(int size)
{
	/* Alway multiple of 8. */
	return ((size-1) | 7) + 1;
}

struct map *safe key_alloc(void)
{
	struct map *m = calloc(1, sizeof(*m));

	key_add(m, "keymap:list", &keymap_list);
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

static void hash_str(const char *key safe, int len, unsigned int *hashes safe)
{
	int i;
	int h = 0;

	for (i = 0; (len < 0 || i < len) && key[i]; i++) {
		h = qhash(key[i], h);
		if (key[i] == '-' || key[i] == ':')
			break;
	}
	hashes[1] = h;
	for (; (len < 0 || i < len) && key[i]; i++)
		h = qhash(key[i], h);
	hashes[0] = h;
}

inline static void set_bit(unsigned long *set safe, int bit)
{
	set[bit/(sizeof(unsigned long)*8)] |=
		1UL << (bit % (sizeof(unsigned long)*8));
}

inline static int test_bit(unsigned long *set safe, int bit)
{
	return !! (set[bit/(sizeof(unsigned long)*8)] &
		   (1UL << (bit % (sizeof(unsigned long)*8))));
}


static int key_present(struct map *map safe, unsigned int *hashes safe)
{
	if (map->changed) {
		int i;
		for (i = 0; i < map->size; i++) {
			unsigned int h[2];
			hash_str(map->keys[i], -1, h);
			if (IS_RANGE(map->comms[i])) {
				set_bit(map->bloom, h[1]&0xff);
				set_bit(map->bloom, (h[1]>>8)&0xff);
				set_bit(map->bloom, (h[1]>>16)&0xff);
			} else {
				set_bit(map->bloom, h[0]&0xff);
				set_bit(map->bloom, (h[0]>>8)&0xff);
				set_bit(map->bloom, (h[0]>>16)&0xff);
			}
		}
		map->changed = 0;
	}

	if (test_bit(map->bloom, hashes[0]&0xff) &&
	    test_bit(map->bloom, (hashes[0]>>8)&0xff) &&
	    test_bit(map->bloom, (hashes[0]>>16)&0xff))
		return 1;
	if (test_bit(map->bloom, hashes[1]&0xff) &&
	    test_bit(map->bloom, (hashes[1]>>8)&0xff) &&
	    test_bit(map->bloom, (hashes[1]>>16)&0xff))
		return 1;
	return 0;
}

/* Find first entry >= k */
static int key_find_len(struct map *map safe, const char *k safe, int len)
{
	int lo = 0;
	int hi = map->size;

	/* all entries before 'lo' are < k.
	 * all entries at 'hi' or later are >= k.
	 * So when lo==hi, hi is the answer.
	 */
	while (hi > lo) {
		int mid = (hi + lo)/ 2;
		int cmp = strncmp(map->keys[mid], k, len);
		if (cmp < 0)
			lo = mid+1;
		else
			hi = mid;
	}
	return hi;
}

static int key_find(struct map *map safe, const char *k safe)
{
	return key_find_len(map, k, strlen(k));
}

void key_add(struct map *map safe, const char *k safe, struct command *comm)
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
			/* Changing the start of a range,
			 * insert an exact match */
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

void key_add_range(struct map *map safe,
		   const char *first safe, const char *last safe,
		   struct command *comm)
{
	int size, move_size;
	int pos, pos2;
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
	 * Then insert a stand-alone for 'last' and update 'pos' to be a
	 * range-start.
	 */
	if (pos2 - 1 > pos && IS_RANGE(map->comms[pos2-1])) {
		free(map->keys[pos2-1]);
		map->keys[pos2-1] = strdup(last);
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

int key_pfx_func(const struct cmd_info *ci safe)
{
	struct pfx_cmd *m = container_of(ci->comm, struct pfx_cmd, c);

	call("Mode:set-all", ci->focus, ci->num, NULL, m->pfx, ci->num2);
	return 1;
}

struct command *key_lookup_cmd(struct map *m safe, const char *c safe,
			       const char **cret, unsigned int *lenret)
{
	/* If 'k' contains an ASCII US (Unit Separator, 0o37 0x1f 31),
	 * it represents multiple keys.
	 * Call key_find() on each of them until success.
	 */
	while (*c) {
		const char *end = strchr(c, '\037');
		int pos;

		if (!end)
			end = c + strlen(c);
		pos = key_find_len(m, c, end - c);

		if (pos >= m->size)
			;
		else if (strncmp(m->keys[pos], c, end - c) == 0 &&
			 m->keys[pos][end - c] == '\0') {
			/* Exact match - use this entry */
			if (cret)
				*cret = c;
			if (lenret)
				*lenret = end - c;
			return GETCOMM(m->comms[pos]);
		} else if (pos > 0 && IS_RANGE(m->comms[pos-1])) {
			/* In a range, use previous */
			if (cret)
				*cret = c;
			if (lenret)
				*lenret = end - c;
			return GETCOMM(m->comms[pos-1]);
		}
		c = end;
		while (*c == '\037')
			c++;
	}
	return NULL;
}

int key_lookup(struct map *m safe, const struct cmd_info *ci safe)
{
	struct command *comm;
	const char *key;
	unsigned int len;

	if (ci->hash && !key_present(m, ci->hash)) {
		stat_count("bloom-miss");
		return Efallthrough;
	}

	comm = key_lookup_cmd(m, ci->key, &key, &len);
	if (comm == NULL || key == NULL) {
		stat_count("bloom-hit-bad");
		return Efallthrough;
	} else {
		/* This is message, but when there are multiple
		 * keys, we need to pass down the one that was matched.
		 */
		int ret;
		const char *oldkey = ci->key;
		char ktmp[40], *k2 = NULL;

		stat_count("bloom-hit-good");
		if (key[len] == 0) {
			((struct cmd_info*)ci)->key = key;
		} else if (len >= sizeof(ktmp)) {
			k2 = strndup(key, len);
			((struct cmd_info*)ci)->key = k2;
		} else {
			strncpy(ktmp, key, len);
			ktmp[len] = 0;
			((struct cmd_info*)ci)->key = ktmp;
		}
		((struct cmd_info*)ci)->comm = comm;
		if (comm->func == keymap_list_func)
			((struct cmd_info*)ci)->comm = (struct command *safe)m;
		ret = comm->func(ci);
		((struct cmd_info*)ci)->key = oldkey;
		free(k2);
		return ret;
	}
}

int key_lookup_prefix(struct map *m safe, const struct cmd_info *ci safe)
{
	int pos = key_find(m, ci->key);
	struct command *comm, *prev = NULL;
	int len = strlen(ci->key);
	const char *k = ci->key;

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
	unsigned int hash[2];

	time_start_key(ci->key);
	if ((void*) ci->comm) {
		int ret = ci->comm->func(ci);
		time_stop_key(ci->key);
		return ret;
	}

	/* FIXME multi-keys */
	if (strchr(ci->key, '\037') == NULL) {
		hash_str(ci->key, -1, hash);
		vci->hash = hash;
	}

	/* If 'home' is set, search from there, else search
	 * from focus
	 */
	p = ci->home;
	if (!p)
		p = ci->focus;

	while (p) {
		int ret = 0;
		if (p->handle && !(p->damaged & DAMAGED_DEAD)) {
			vci->home = p;
			vci->comm = p->handle;
			ret = p->handle->func(ci);
		}
		if (ret) {
			time_stop_key(ci->key);
			/* 'p' might have been destroyed */
			return ret;
		}
		if (p == p->parent)
			p = NULL;
		else
			p = p->parent;
	}
	time_stop_key(ci->key);
	return 0;
}
