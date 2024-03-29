/*
 * Copyright Neil Brown ©2015-2023 <neil@brown.name>
 * May be distributed under terms of GPLv2 - see file:COPYING
 *
 * Assorted utility functions used by edlib
 *
 */
#ifndef EDLIB_MISC
#define EDLIB_MISC

#include <wchar.h>
#include "list.h"

#undef bool
typedef _Bool bool;
#define True ((bool)1)
#define False ((bool)0)

#define strstarts(s, prefix) (strncmp(s, prefix, sizeof(prefix)-1) == 0)

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

/* Formatting can be embedded in text strings in some places using
 * SOH STX ETX.
 * SOH format-attributes STX the text ETX
 * "the text" can contain nested SOH/STX/ETX sequences.
 * The same can be done with
 *  < format-attributes> the text </a>
 * but that is being phased out.
 */
#define SOH "\001"
#define STX "\002"
#define ETX "\003"
#define soh '\001'
#define stx '\002'
#define etx '\003'
/* ACK is used as a no-op. */
#define ack '\006'
#define ACK "\006"

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
	TIME__COUNT,
};
void time_start(enum timetype);
void time_stop(enum timetype);
void time_start_key(const char *key safe);
void time_stop_key(const char *key safe);

extern bool debugger_is_present(void);

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

#define MEMPOOL(_name)							\
	struct mempool mem ## _name = {				\
		.name = #_name,					\
		.linkage = LIST_HEAD_INIT(mem ## _name.linkage),	\
	}
#define MEMPOOL_DECL(_name) struct mempool mem ## _name;

void *safe do_alloc(struct mempool *pool safe, int size, int zero);
void do_unalloc(struct mempool *pool safe, const void *obj, int size);

#define alloc(var, pool)						\
	do { var = do_alloc(&mem##pool, sizeof((var)[0]), 1); } while (0)

#define alloc_buf(size, pool) do_alloc(&mem##pool, size, 0)
#define alloc_zbuf(size, pool) do_alloc(&mem##pool, size, 1)

#define unalloc(var, pool)						\
	do { do_unalloc(&mem##pool, var, sizeof((var)[0])); (var)=NULL; } while (0)

#define unalloc_buf(var, size, pool)					\
	do { do_unalloc(&mem##pool, var, size); (var) = NULL; } while(0)

#define unalloc_str(var, pool)						\
	unalloc_buf(var, strlen(var)+1, pool)

#define unalloc_safe(var, pool)						\
	do { do_unalloc(&mem##pool, var, sizeof((var)[0])); (var)=safe_cast NULL; } while (0)

#define unalloc_buf_safe(var, size, pool)				\
	do { do_unalloc(&mem##pool, var, size); (var) = safe_cast NULL; } while(0)

#define unalloc_str_safe(var, pool)						\
	unalloc_buf_safe(var, strlen(var)+1, pool)

/* attrs parsing */

/* Sequentially set _attr to the an attr name, and _val to
 * either the val (following ":") or NULL.
 * _attr is valid up to : or , or < space and _val is valid up to , or <space
 * _c is the start which will be updates, and _end is the end which
 * must point to , or nul or a control char
 */
#define foreach_attr(_attr, _val, _c, _end)			\
	for (_attr = _c, _val = afind_val(&_c, _end);		\
	     _attr;						\
	     _attr = _c, _val = afind_val(&_c, _end))
const char *afind_val(const char **cp safe, const char *end);
char *aupdate(char **cp safe, const char *v);
bool amatch(const char *a safe, const char *m safe);
bool aprefix(const char *a safe, const char *m safe);
long anum(const char *v safe);

#endif /* EDLIB_MISC */
