/*
 * Copyright Neil Brown ©2015-2016 <neil@brown.name>
 * May be distributed under terms of GPLv2 - see file:COPYING
 *
 * Assorted utility functions used by edlib
 *
 */
#define _GNU_SOURCE /*  for asprintf */
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include <time.h>
#include <unistd.h>

#include "safe.h"

#include "misc.h"

void buf_init(struct buf *b safe)
{
	b->b = safe_cast NULL;
	b->len = 0;
	b->size = 0;
}

void buf_concat_len(struct buf *b safe, char *s safe, int l)
{

	if (b->len + l >= b->size) {
		while (b->len + l >= b->size)
			b->size += 128;
		b->b = realloc(b->b, b->size);
	}
	strncpy(b->b + b->len, s, l);
	b->len += l;
	b->b[b->len] = 0;
}

void buf_concat(struct buf *b safe, char *s safe)
{
	int l = strlen(s);
	buf_concat_len(b, s, l);
}

void buf_append(struct buf *b safe, wchar_t wch)
{
	char t[5];
	mbstate_t ps = {};
	size_t l;

	if (wch <= 0x7f) {
		t[0] = wch;
		l = 1;
	} else
		l = wcrtomb(t, wch, &ps);
	buf_concat_len(b, t, l);
}

void buf_append_byte(struct buf *b safe, char c)
{
	buf_concat_len(b, &c, 1);
}


/*
 * performance measurements
 */

static long long tstart[__TIME_COUNT];
static int tcount[__TIME_COUNT];
static long long tsum[__TIME_COUNT];
static int stats_enabled = 1;

static time_t last_dump = 0;
static FILE *dump_file;

static void dump_key_hash(void);
static void dump_count_hash(void);

char *tnames[] = {
	[TIME_KEY]     = "KEY",
	[TIME_WINDOW]  = "WINDOW",
	[TIME_READ]    = "READ",
	[TIME_SIG]     = "SIG",
	[TIME_TIMER]   = "TIMER",
	[TIME_IDLE]    = "IDLE",
	[TIME_REFRESH] = "REFRESH",
};

#define NSEC 1000000000
void time_start(enum timetype type)
{
	struct timespec start;
	if (type < 0 || type >= __TIME_COUNT || !stats_enabled)
		return;
	clock_gettime(CLOCK_MONOTONIC, &start);
	tstart[type] = start.tv_sec * NSEC + start.tv_nsec;
}

void time_stop(enum timetype type)
{
	struct timespec stop;
	long long nsec;
	int i;

	if (type < 0 || type >= __TIME_COUNT || !stats_enabled)
		return;
	clock_gettime(CLOCK_MONOTONIC, &stop);

	nsec = (stop.tv_sec * NSEC + stop.tv_nsec) - tstart[type];
	tcount[type] += 1;
	tsum[type] += nsec;

	if (stop.tv_sec < last_dump + 30 || tcount[TIME_REFRESH] < 100)
		return;
	if (last_dump == 0) {
		last_dump = stop.tv_sec;
		return;
	}
	if (!getenv("EDLIB_STATS")) {
		stats_enabled = 0;
		return;
	}

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
	fprintf(dump_file, "%ld:", (long)stop.tv_sec);
	for (i = 0; i< __TIME_COUNT; i++) {
		fprintf(dump_file, " %s:%d:%lld", tnames[i], tcount[i],
		        tsum[i] / (tcount[i]?:1));
		tcount[i] = 0;
		tsum[i] = 0;
	}
	dump_key_hash();
	dump_count_hash();
	fprintf(dump_file, "\n");
	fflush(dump_file);
	last_dump = stop.tv_sec;
}

inline static int qhash(char key, unsigned int start)
{
	return (start ^ key) * 0x61C88647U;
}

static int hash_str(char *key safe, int len)
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

struct khash *khashtab[1024];

struct kstack {
	long long tstart;
	char *name;
} kstack[20];
int ktos = 0;

void time_start_key(char *key)
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

static struct khash *hash_find(struct khash **table, char *key)
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

void time_stop_key(char *key)
{
	struct timespec stop;
	struct khash *h;

	if (!stats_enabled)
		return;
	if (ktos <= 0)
		abort();
	ktos -= 1;
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

struct khash *count_tab[1024];

void stat_count(char *name)
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

