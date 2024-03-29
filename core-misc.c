/*
 * Copyright Neil Brown ©2015-2023 <neil@brown.name>
 * May be distributed under terms of GPLv2 - see file:COPYING
 *
 * Assorted utility functions used by edlib
 *
 */
#define _GNU_SOURCE /*  for asprintf */
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <wchar.h>
#include <time.h>
#include <unistd.h>

#include "core.h"
#include "safe.h"
#include "list.h"
#include "misc.h"

void buf_init(struct buf *b safe)
{
	b->size = 32;
	b->b = malloc(b->size);
	b->len = 0;
}

void buf_resize(struct buf *b safe, int size)
{
	size += 1; /* we will nul-terminate */
	if (size > b->size) {
		b->size = size;
		b->b = realloc(b->b, b->size);
	}
}

void buf_concat_len(struct buf *b safe, const char *s safe, int l)
{

	if (b->len + l >= b->size) {
		while (b->len + l >= b->size)
			b->size += 128;
		b->b = realloc(b->b, b->size);
	}
	memcpy(b->b + b->len, s, l);
	b->len += l;
	b->b[b->len] = 0;
}

void buf_concat(struct buf *b safe, const char *s safe)
{
	int l = strlen(s);
	buf_concat_len(b, s, l);
}

void buf_append(struct buf *b safe, wchar_t wch)
{
	char t[5];

	buf_concat(b, put_utf8(t, wch));
}

void buf_append_byte(struct buf *b safe, char c)
{
	buf_concat_len(b, &c, 1);
}

/*
 * performance measurements
 */

static long long tstart[TIME__COUNT];
static int tcount[TIME__COUNT];
static long long tsum[TIME__COUNT];
static int stats_enabled = 1;

static time_t last_dump = 0;
static FILE *dump_file;

static void dump_key_hash(void);
static void dump_count_hash(void);
static void stat_dump(void);
static void dump_mem(void);

static const char *tnames[] = {
	[TIME_KEY]     = "KEY",
	[TIME_WINDOW]  = "WINDOW",
	[TIME_READ]    = "READ",
	[TIME_SIG]     = "SIG",
	[TIME_TIMER]   = "TIMER",
	[TIME_IDLE]    = "IDLE",
	[TIME_REFRESH] = "REFRESH",
	[TIME_MISC]    = "MISC",
};

#define NSEC 1000000000
void time_start(enum timetype type)
{
	struct timespec start;
	if (type < 0 || type >= TIME__COUNT || !stats_enabled)
		return;
	clock_gettime(CLOCK_MONOTONIC, &start);
	tstart[type] = start.tv_sec * NSEC + start.tv_nsec;
}

void time_stop(enum timetype type)
{
	struct timespec stop;
	long long nsec;

	if (type < 0 || type >= TIME__COUNT || !stats_enabled)
		return;
	if (!tstart[type])
		return;
	clock_gettime(CLOCK_MONOTONIC, &stop);

	nsec = (stop.tv_sec * NSEC + stop.tv_nsec) - tstart[type];
	tstart[type] = 0;
	tcount[type] += 1;
	tsum[type] += nsec;

	if (getenv("EDLIB_STATS_FAST"))	{
		if (stop.tv_sec < last_dump + 5 || tcount[TIME_REFRESH] < 10)
			return;
	} else {
		if (stop.tv_sec < last_dump + 30 || tcount[TIME_REFRESH] < 100)
			return;
	}
	if (last_dump == 0) {
		last_dump = stop.tv_sec;
		return;
	}
	if (!getenv("EDLIB_STATS")) {
		stats_enabled = 0;
		return;
	}
	last_dump = stop.tv_sec;
	stat_dump();
}

static void stat_dump(void)
{
	int i;

	if (!dump_file) {
		char *fname = NULL;
		asprintf(&fname, ".edlib_stats-%d", getpid());
		if (!fname)
			fname = "/tmp/edlib_stats";
		dump_file = fopen(fname, "w");
		if (!dump_file) {
			stats_enabled = 0;
			return;
		}
	}
	fprintf(dump_file, "%ld:", (long)time(NULL));
	for (i = 0; i< TIME__COUNT; i++) {
		fprintf(dump_file, " %s:%d:%lld", tnames[i], tcount[i],
			tsum[i] / (tcount[i]?:1));
		tcount[i] = 0;
		tsum[i] = 0;
	}
	dump_key_hash();
	dump_count_hash();
	fprintf(dump_file, "\n");
	dump_mem();
	fflush(dump_file);
}

inline static int qhash(char key, unsigned int start)
{
	return (start ^ key) * 0x61C88647U;
}

static int hash_str(const char *key safe, int len)
{
	int i;
	int h = 0;

	for (i = 0; (len < 0 || i < len) && key[i]; i++)
		h = qhash(key[i], h);
	return h;
}

struct khash {
	struct khash *next;
	int hash;
	long long tsum;
	int tcount;
	char name[1];
};

static struct khash *khashtab[1024];

static struct kstack {
	long long tstart;
	const char *name;
} kstack[20];
static int ktos = 0;

void time_start_key(const char *key safe)
{
	struct timespec start;

	if (!stats_enabled)
		return;
	ktos += 1;
	if (ktos > 20)
		return;
	clock_gettime(CLOCK_MONOTONIC, &start);
	kstack[ktos-1].tstart = start.tv_sec * NSEC + start.tv_nsec;
	kstack[ktos-1].name = key;
}

static struct khash *hash_find(struct khash **table, const char *key safe)
{
	struct khash *h, **hp;
	int hash;

	hash = hash_str(key, -1);
	hp = &table[hash & 1023];
	while ( (h = *hp) && (h->hash != hash || strcmp(h->name, key) != 0))
		hp = &h->next;
	if (!h) {
		h = malloc(sizeof(*h) + strlen(key));
		h->hash = hash;
		h->tsum = 0;
		h->tcount = 0;
		strcpy(h->name, key);
		h->next = *hp;
		*hp = h;
	}
	return h;
}

void time_stop_key(const char *key safe)
{
	struct timespec stop;
	struct khash *h;

	if (!stats_enabled)
		return;
	if (ktos <= 0)
		abort();
	ktos -= 1;
	if (ktos >= 20)
		return;
	if (key != kstack[ktos].name)
		abort();
	clock_gettime(CLOCK_MONOTONIC, &stop);

	h = hash_find(khashtab, key);
	h->tcount += 1;
	h->tsum += stop.tv_sec * NSEC + stop.tv_nsec - kstack[ktos].tstart;
}

static void dump_key_hash(void)
{
	int i;
	int cnt = 0;
	int buckets = 0;
	int max = 0;

	for (i = 0; i < 1024; i++) {
		struct khash *h;
		int c = 0;
		for (h = khashtab[i]; h ; h = h->next) {
			c += 1;
			if (!h->tcount)
				continue;
			fprintf(dump_file, " %s:%d:%lld",
				h->name, h->tcount,
				h->tsum / (h->tcount?:1));
			h->tcount = 0;
			h->tsum = 0;
		}
		cnt += c;
		buckets += !!c;
		if (c > max)
			max = c;
	}
	fprintf(dump_file, " khash:%d:%d:%d", cnt, buckets, max);
}

static struct khash *count_tab[1024];

void stat_count(char *name safe)
{
	struct khash *h;

	if (!stats_enabled)
		return;
	h = hash_find(count_tab, name);
	h->tcount += 1;
}

static void dump_count_hash(void)
{
	int i;
	int cnt = 0;
	int buckets = 0;
	int max = 0;

	for (i = 0; i < 1024; i++) {
		struct khash *h;
		int c = 0;
		for (h = count_tab[i]; h ; h = h->next) {
			c += 1;
			fprintf(dump_file, " %s:%d:-",
				h->name, h->tcount);
			h->tcount = 0;
			h->tsum = 0;
		}
		cnt += c;
		buckets += !!c;
		if (c > max)
			max = c;
	}
	fprintf(dump_file, " nhash:%d:%d:%d", cnt, buckets, max);
}

static void hash_free(struct khash **tab safe)
{
	int i;

	for (i = 0; i < 1024; i++) {
		struct khash *h;

		while ((h = tab[i]) != NULL) {
			tab[i] = h->next;
			free(h);
		}
	}
}

void stat_free(void)
{
	/* stats_enabled is only valid after 30 seconds, so
	 * so we need to check EDLIB_STATS directly
	 */
	if (stats_enabled && getenv("EDLIB_STATS"))
		stat_dump();
	hash_free(count_tab);
	hash_free(khashtab);
	stats_enabled = 0;
}

static LIST_HEAD(mem_pools);

void *safe do_alloc(struct mempool *pool safe, int size, int zero)
{
	void *ret = malloc(size);

	if (zero)
		memset(ret, 0, size);
	pool->bytes += size;
	pool->allocations += 1;
	if (pool->bytes > pool->max_bytes)
		pool->max_bytes = pool->bytes;
	if (list_empty(&pool->linkage))
		list_add(&pool->linkage, &mem_pools);
	return ret;
}

void do_unalloc(struct mempool *pool safe, const void *obj, int size)
{
	if (obj) {
		pool->bytes -= size;
		pool->allocations -= 1;
		free((void*)obj);
	}
}

static void dump_mem(void)
{
	struct mempool *p;

	fprintf(dump_file, "mem:");
	list_for_each_entry(p, &mem_pools, linkage)
		fprintf(dump_file, " %s:%ld(%ld):%ld",
			p->name, p->bytes, p->max_bytes, p->allocations);
	fprintf(dump_file, "\n");
}

/* UTF-8 handling....
 * - return wchar (wint_t) and advance pointer
 * - append encoding to buf, advance pointer, decrease length
 *
 * UTF-8:
 * - if it starts '0b0', it is a 7bit code point
 * - if it starts '0b10' it is a non-initial byte and provides 6 bits.
 * - if it starts '0b110' it is first of 2 and provides 5 of 11 bits
 * - if it starts '0b1110' it is first of 3 and provides 4 of 16 bits
 * - if it starts '0b11110' it is first of 4 and provides 3 of 21 bits.
 */
wint_t get_utf8(const char **cpp safe, const char *end)
{
	int tail = 0;
	wint_t ret = 0;
	const char *cp = *cpp;
	unsigned char c;

	if (!cp)
		return WEOF;
	if (end && end <= cp)
		return WEOF;
	c = (unsigned char)*cp++;
	if (!c)
		return WEOF;
	if (c < 0x80)
		ret = c;
	else if (c < 0xc0)
		return WERR;
	else if (c < 0xe0) {
		ret = c & 0x1f;
		tail = 1;
	} else if (c < 0xf0) {
		ret = c & 0xf;
		tail = 2;
	} else if (c < 0xf8) {
		ret = c & 0x7;
		tail = 3;
	} else
		return WERR;
	if (end && end < cp + tail)
		return WERR;
	while (tail--) {
		c = *cp++;
		if ((c & 0xc0) != 0x80)
			return WERR;
		ret = (ret << 6) | (c & 0x3f);
	}
	*cpp = cp;
	return ret;
}

char *safe put_utf8(char *buf safe, wchar_t ch)
{
	char mask;
	int l, i;

	if (ch < 0x80) {
		l = 1;
		mask = 0x7f;
	} else if (ch < 0x800) {
		l = 2;
		mask = 0x1f;
	} else if (ch < 0x10000) {
		l = 3;
		mask = 0x0f;
	} else if (ch < 0x200000) {
		l = 4;
		mask = 0x07;
	} else
		l = 0;

	for (i = 0 ; i < l; i++) {
		buf[i] = (ch >> ((l-1-i)*6)) & mask;
		buf[i] |= ~(mask+mask+1);
		mask = 0x3f;
	}
	buf[l] = 0;
	return buf;
}

int utf8_strlen(const char *s safe)
{
	int cnt = 0;

	while (*s) {
		if ((*s & 0xc0) != 0x80)
			cnt += 1;
		s += 1;
	}
	return cnt;
}

int utf8_strnlen(const char *s safe, int n)
{
	int cnt = 0;

	while (*s && n > 0) {
		if ((*s & 0xc0) != 0x80)
			cnt += 1;
		s += 1;
		n -= 1;
	}
	return cnt;
}

int utf8_valid(const char *s safe)
{
	wint_t c;

	while ((c = get_utf8(&s, NULL)) != WEOF) {
		if (c == WERR ||
		    c > 0x10FFFF)
			return 0;
	}
	return 1;
}

/*
 * When walking backwards through a string, we need to round a point
 * down to the start of a code-point.
 * When reading a file into allocated chunks of memory, we want each chunk
 * to hold a whole number of code points.
 * For both of these needs, we have utf8_round_len which tries to reduce
 * the given length to a code-point boundary, if possible.
*
 * We only adjust the length if we can find a start-of-code-point in
 * the last 4 bytes. (longest UTF-8 encoding of 21bit unicode is 4 bytes).
 * A start of codepoint starts with 0b0 or 0b11, not 0b10.
 */
int utf8_round_len(const char *text safe, int len)
{
	/* The string at 'text' is *longer* than 'len', or
	 * at least text[len] is defined - it can be nul.  If
	 * [len] isn't the start of a new codepoint, and there
	 * is a start marker in the previous 4 bytes,
	 * move back to there.
	 */
	int i = 0;
	while (i <= len && i <=4)
		if ((text[len-i] & 0xC0) == 0x80)
			/* next byte is inside a UTF-8 code point, so
			 * this isn't a good spot to end. Try further
			 * back */
			i += 1;
		else
			return len-i;
	return len;
}

static int _debugger_present = -1;
static void _sigtrap_handler(int signum)
{
	_debugger_present = 0;
	signal(SIGTRAP, SIG_DFL);
}

bool debugger_is_present(void)
{
	if (_debugger_present < 0) {
		_debugger_present = 1;
		signal(SIGTRAP, _sigtrap_handler);
		raise(SIGTRAP);
	}
	return _debugger_present;
}

/* attr parsing */
const char *afind_val(const char **cp safe, const char *end)
{
	const char *c = *cp;
	const char *ret;

	if (!c)
		return NULL;
	if (!end)
		end = c + strlen(c);
	while (c < end && *c != ':' && *c != ',')
		c++;
	if (c == end) {
		*cp = NULL;
		return NULL;
	}
	if (*c == ',') {
		while (*c == ',' && c < end)
			c++;
		if (c == end) {
			*cp = NULL;
			return NULL;
		}
		*cp = c;
		return NULL;
	}
	c += 1;
	ret = c;
	while (c < end && *c != ',')
		c++;
	while (c < end && *c == ',')
		c++;
	if (c == end)
		c = NULL;
	*cp = c;
	return ret;
}

char *aupdate(char **cp safe, const char *v)
{
	/* duplicate value at v and store in *cp, freeing what is there
	 * first
	 */
	const char *end = v;

	while (end && *end != ',' && *end >= ' ')
		end += 1;

	free(*cp);
	if (v)
		*cp = strndup(v, end-v);
	else
		*cp = NULL;
	return *cp;
}

bool amatch(const char *a safe, const char *m safe)
{
	while (*a && *a == *m) {
		a += 1;
		m += 1;
	}
	if (*m)
		/* Didn't match all of m */
		return False;
	if (*a != ':' && *a != ',' && *a >= ' ')
		/* Didn't match all of a */
		return False;
	return True;
}

bool aprefix(const char *a safe, const char *m safe)
{
	while (*a && *a == *m) {
		a += 1;
		m += 1;
	}
	if (*m)
		/* Didn't match all of m */
		return False;
	return True;
}

long anum(const char *v safe)
{
	char *end = NULL;
	long ret = strtol(v, &end, 10);
	if (end == v || !end ||
	    (*end != ',' && *end >= ' '))
		/* Not a valid number - use zero */
		return 0;
	return ret;
}
