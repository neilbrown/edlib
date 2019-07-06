/*
 * Copyright Neil Brown ©2015-2016 <neil@brown.name>
 * May be distributed under terms of GPLv2 - see file:COPYING
 *
 * Assorted utility functions used by edlib
 *
 */

struct buf {
	char *b safe;
	int len;
	int size;
};

void buf_init(struct buf *b safe);
void buf_concat(struct buf *b safe, char *s safe);
void buf_concat_len(struct buf *b safe, char *s safe, int l);
void buf_append(struct buf *b safe, wchar_t wch);
void buf_append_byte(struct buf *b safe, char c);
static inline char *safe buf_final(struct buf *b safe)
{
	if ((void*)b->b)
		b->b[b->len] = 0;
	return b->b;
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
	__TIME_COUNT,
};
void time_start(enum timetype);
void time_stop(enum timetype);
void time_start_key(char *key);
void time_stop_key(char *key);

void stat_count(char *name);
