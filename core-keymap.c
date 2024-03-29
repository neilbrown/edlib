/*
 * Copyright Neil Brown ©2015-2023 <neil@brown.name>
 * May be distributed under terms of GPLv2 - see file:COPYING
 *
 * Keymaps for edlib.
 *
 * A keymap maps a key to a command.
 * Keys are ordered for fast binary-search lookup.
 * A "key" is an arbitrary string which typically contains
 * some 'mode' prefix and some specific tail.
 * e.g emacs:A:C-X is Alt-Control-X in emacs mode.
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

#define _GNU_SOURCE /*  for asprintf */
#include <unistd.h>
#include <stdlib.h>
#include <memory.h>
#include <stdio.h>

#include "core.h"
#include "misc.h"

inline static int qhash(char key, unsigned int start)
{
	return (start ^ key) * 0x61C88647U;
}

struct map {
	unsigned long	bloom[256 / (sizeof(unsigned long)*8) ];
	bool		changed;
	bool		check_all;
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
		return Efallthrough;
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

static bool hash_str(const char *key safe, int len, unsigned int *hashes safe)
{
	int i;
	int h = 0;
	bool prefix = False;

	for (i = 0; (len < 0 || i < len) && key[i]; i++) {
		h = qhash(key[i], h);
		if (key[i] == '-' || key[i] == ':') {
			prefix = True;
			break;
		}
	}
	hashes[1] = h;
	for (; (len < 0 || i < len) && key[i]; i++)
		h = qhash(key[i], h);
	hashes[0] = h;
	return prefix;
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

static bool key_present(struct map *map safe, unsigned int *hashes safe)
{
	if (map->changed) {
		int i;
		map->check_all = False;
		for (i = 0; i < map->size; i++) {
			unsigned int h[2];
			bool prefix = hash_str(map->keys[i], -1, h);
			if (IS_RANGE(map->comms[i])) {
				if (!prefix)
					map->check_all = True;
				set_bit(map->bloom, h[1]&0xff);
				set_bit(map->bloom, (h[1]>>8)&0xff);
				set_bit(map->bloom, (h[1]>>16)&0xff);
			} else {
				set_bit(map->bloom, h[0]&0xff);
				set_bit(map->bloom, (h[0]>>8)&0xff);
				set_bit(map->bloom, (h[0]>>16)&0xff);
			}
		}
		map->changed = False;
	}
	if (map->check_all)
		return True;

	if (test_bit(map->bloom, hashes[0]&0xff) &&
	    test_bit(map->bloom, (hashes[0]>>8)&0xff) &&
	    test_bit(map->bloom, (hashes[0]>>16)&0xff))
		return True;
	if (test_bit(map->bloom, hashes[1]&0xff) &&
	    test_bit(map->bloom, (hashes[1]>>8)&0xff) &&
	    test_bit(map->bloom, (hashes[1]>>16)&0xff))
		return True;
	return False;
}

/* Find first entry >= k */
static int key_find(struct map *map safe, const char *k safe)
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

void key_add(struct map *map safe, const char *k safe, struct command *comm)
{
	int size;
	int pos;
	struct command *comm2 = NULL;
	int ins_cnt;

	if (!comm)
		return;
	if (strcmp(k, "Close") == 0 &&
	    !comm->closed_ok) {
		LOG("WARNING: Command %s registered for \"Close\" but not marked closed_ok",
		    comm->name);
	}

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
	map->changed = True;
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
	map->changed = True;
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
	map->changed = True;
}
#endif

int key_pfx_func(const struct cmd_info *ci safe)
{
	struct pfx_cmd *m = container_of(ci->comm, struct pfx_cmd, c);

	call("Mode:set-all", ci->focus, ci->num, NULL, m->pfx, ci->num2);
	return 1;
}

struct command *key_lookup_cmd(struct map *m safe, const char *c safe)
{
	int pos = key_find(m, c);

	if (pos >= m->size)
		;
	else if (strcmp(m->keys[pos], c) == 0)
		/* Exact match - use this entry */
		return GETCOMM(m->comms[pos]);
	else if (pos > 0 && IS_RANGE(m->comms[pos-1]))
		/* In a range, use previous */
		return GETCOMM(m->comms[pos-1]);

	return NULL;
}

/* FIXME this makes lots of things non re-entrant */
static struct backtrace {
	struct command *comm safe;
	const struct cmd_info *ci safe;
	struct backtrace *prev;
} *backtrace;
static int backtrace_depth;

static char *mark_info(struct mark *m)
{
	char *ret = NULL;

	if (!m) {
		asprintf(&ret, "M-");
		return ret;
	}
	if (!mark_valid(m)) {
		asprintf(&ret, "M-FREED");
		return ret;
	}
	ret = pane_call_ret(str, m->owner, "doc:debug:mark",
			    m->owner, 0, m);
	if (ret)
		return ret;

	asprintf(&ret, "M:%d<%p>%d", m->seq, m, m->ref.i);
	return ret;
}

void LOG_BT(void)
{
	struct backtrace *bt;
	LOG("Start Backtrace:");
	for (bt = backtrace; bt; bt = bt->prev) {
		const struct cmd_info *ci = bt->ci;
		struct command *h = ci->home->handle;
		struct command *f = ci->focus->handle;
		char *m1 = mark_info(ci->mark);
		char *m2 = mark_info(ci->mark2);

		LOG(" H:%s \"%s\" F:%s: %d %s \"%s\" %d %s \"%s\" (%d,%d) %s",
		    h ? h->name : "?",
		    ci->key,
		    f ? f->name : "?",
		    ci->num, m1, ci->str,
		    ci->num2, m2, ci->str2,
		    ci->x, ci->y,
		    ci->comm2 ? ci->comm2->name : "");
		free(m1);
		free(m2);
	}
	LOG("End Backtrace");
}

int do_comm_call(struct command *comm safe, const struct cmd_info *ci safe)
{
	struct backtrace bt;
	int ret;

	if (ci->home->damaged & DAMAGED_DEAD)
		return Efail;
	if (times_up_fast(ci->home))
		return Efail;
	if ((ci->home->damaged & DAMAGED_CLOSED) &&
	    !comm->closed_ok)
		return Efallthrough;

	if (backtrace_depth > 100) {
		backtrace_depth = 0;
		LOG("Recursion limit of 100 reached");
		LOG_BT();
		backtrace_depth = 100;
		pane_root(ci->home)->timestamp = 1;
		return Efail;
	}
	bt.comm = comm;
	bt.ci = ci;
	bt.prev = backtrace;
	backtrace = &bt;
	backtrace_depth += 1;
	ret = comm->func(ci);
	backtrace = bt.prev;
	backtrace_depth -= 1;
	return ret;
}

int key_lookup(struct map *m safe, const struct cmd_info *ci safe)
{
	struct command *comm;

	if (ci->hash && !key_present(m, ci->hash)) {
		stat_count("bloom-miss");
		return Efallthrough;
	}

	comm = key_lookup_cmd(m, ci->key);
	if (comm == NULL) {
		stat_count("bloom-hit-bad");
		return Efallthrough;
	} else {
		stat_count("bloom-hit-good");
		((struct cmd_info*)ci)->comm = comm;

		if (comm->func == keymap_list_func)
			((struct cmd_info*)ci)->comm = (struct command *safe)m;

		return do_comm_call(comm, ci);
	}
}

int key_lookup_prefix(struct map *m safe, const struct cmd_info *ci safe,
		      bool simple)
{
	/* A "Simple" lookup avoids the backtrace.  It is used in
	 * signal handlers.
	 */
	const char *k = ci->key;
	int len = strlen(k);
	int pos = key_find(m, k);
	struct command *comm, *prev = NULL;
	int ret = Efallthrough;
	int i;

	for (i = 0;
	     ret == Efallthrough && pos+i < m->size &&
	     strncmp(m->keys[pos+i], k, len) == 0;
	     i++) {
		comm = GETCOMM(m->comms[pos+i]);
		if (comm && comm != prev) {
			((struct cmd_info*)ci)->comm = comm;
			((struct cmd_info*)ci)->key = m->keys[pos+i];
			if (simple)
				ret = comm->func(ci);
			else
				ret = do_comm_call(comm, ci);
			ASSERT(ret >= Efallthrough || ret < Eunused);
			prev = comm;
			/* something might have been added, recalc
			 * start pos.
			 */
			pos = key_find(m, k);
		}
	}
	((struct cmd_info*)ci)->key = k;
	return ret;
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

	if (ci->mark && !mark_valid(ci->mark))
		return Einval;
	if (ci->mark2 && !mark_valid(ci->mark2))
		return Einval;

	if (times_up(ci->home))
		return Efail;
	time_start_key(ci->key);
	if ((void*) ci->comm) {
		int ret = do_comm_call(ci->comm, ci);
		time_stop_key(ci->key);
		return ret;
	}

	hash_str(ci->key, -1, hash);
	vci->hash = hash;

	/* If 'home' is set, search from there, else search
	 * from focus
	 */
	p = ci->home;
	if (!p)
		p = ci->focus;

	while (p) {
		int ret = Efallthrough;
		if (p->handle &&
		    (p->handle->closed_ok ||
		     !(p->damaged & DAMAGED_CLOSED))) {
			vci->home = p;
			vci->comm = p->handle;
			/* Don't add this to the call stack as it
			 * should simply call the desired function and
			 * that will appear on the call stack.
			 */
			ret = p->handle->func(ci);
		}
		if (ret != Efallthrough) {
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
	return Efallthrough;
}
