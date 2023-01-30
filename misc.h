/*
 * Copyright Neil Brown ©2015-2021 <neil@brown.name>
 * May be distributed under terms of GPLv2 - see file:COPYING
 *
 * Assorted utility functions used by edlib
 *
 */
#ifndef EDLIB_MISC
#define EDLIB_MISC

#include "list.h"

#undef bool
typedef _Bool bool;
#define True ((bool)1)
#define False ((bool)0)

#define WERR (0xfffffffeu)
wint_t get_utf8(const char **cpp safe, const char *end);
char *safe put_utf8(char *buf safe, wchar_t ch);
int utf8_strlen(const char *s safe);
int utf8_strnlen(const char *s safe, int n);
int utf8_round_len(const char *text safe, int len);
int utf8_valid(const char *s safe);
static inline int utf8_bytes(wchar_t ch)
{
	if (ch < 0x80)
		return 1;
	else if (ch < 0x800)
		return 2;
	else if (ch < 0x10000)
		return 3;
	else if (ch < 0x200000)
		return 4;
	else
		return 0;
}

struct buf {
	char *b safe;
	int len;
	int size;
};

void buf_init(struct buf *b safe);
void buf_concat(struct buf *b safe, const char *s safe);
void buf_concat_len(struct buf *b safe, const char *s safe, int l);
void buf_append(struct buf *b safe, wchar_t wch);
void buf_append_byte(struct buf *b safe, char c);
void buf_resize(struct buf *b safe, int size);
static inline char *safe buf_final(struct buf *b safe)
{
	b->b[b->len] = 0;
	return b->b;
}
static inline void buf_reinit(struct buf *b safe)
{
	b->len = 0;
}

/* Performance measurements.
 * 1/ timers.
 */
enum timetype {
	TIME_KEY,
	TIME_WINDOW,
	TIME_READ,
	TIME_SIG,
	TIME_TIMER,
	TIME_IDLE,
	TIME_REFRESH,
	TIME_MISC,
	__TIME_COUNT,
};
void time_start(enum timetype);
void time_stop(enum timetype);
void time_start_key(const char *key safe);
void time_stop_key(const char *key safe);

int times_up(void);
void time_starts(void);
void time_ends(void);

void stat_count(char *name safe);

void stat_free(void);

/*
 * Memory allocation tracking
 */

struct mempool {
	char *name;
	long bytes;
	long allocations;
	long max_bytes;
	struct list_head linkage;
};

#define MEMPOOL(__name)							\
	struct mempool mem ## __name = {				\
		.name = #__name,					\
		.linkage = LIST_HEAD_INIT(mem ## __name.linkage),	\
	}
#define MEMPOOL_DECL(__name) struct mempool mem ## __name;

void *safe __alloc(struct mempool *pool safe, int size, int zero);
void __unalloc(struct mempool *pool safe, void *obj, int size);

#define alloc(var, pool)						\
	do { var = __alloc(&mem##pool, sizeof((var)[0]), 1); } while (0)

#define alloc_buf(size, pool) __alloc(&mem##pool, size, 0)

#define unalloc(var, pool)						\
	do { __unalloc(&mem##pool, var, sizeof((var)[0])); (var)=NULL; } while (0)

#define unalloc_buf(var, size, pool)					\
	do { __unalloc(&mem##pool, var, size); (var) = NULL; } while(0)

#define unalloc_safe(var, pool)						\
	do { __unalloc(&mem##pool, var, sizeof((var)[0])); (var)=safe_cast NULL; } while (0)

#define unalloc_buf_safe(var, size, pool)				\
	do { __unalloc(&mem##pool, var, size); (var) = safe_cast NULL; } while(0)

#endif /* EDLIB_MISC */
