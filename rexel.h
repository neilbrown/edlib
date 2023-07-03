/*
 * Copyright Neil Brown ©2015-2023 <neil@brown.name>
 * May be distributed under terms of GPLv2 - see file:COPYING
 */

#include <wchar.h>
#include "safe.h"

struct match_state;
enum rxl_found {
	RXL_NOMATCH,	/* No match has been found at all */
	RXL_CONTINUE,	/* No match here, but it is worth looking further */
	RXL_DONE,	/* A match was previously reported, but no further match
			 * can be found as we are anchored on that match.
			 */
	RXL_MATCH,	/* There was a match once the char was processed */
	RXL_MATCH_FLAG,	/* A match was found due to flags, but not once char was
			 * processed.
			 */
};
unsigned short *rxl_parse(const char *patn safe, int *lenp, int nocase);
unsigned short *safe rxl_parse_verbatim(const char *patn safe, int nocase);

struct match_state *safe rxl_prepare(unsigned short *rxl safe, int flags);
#define	RXLF_ANCHORED	1
#define	RXLF_BACKTRACK	2

int rxl_prefix(unsigned short *rxl safe, char *ret safe, int max);
int rxl_fast_match(const char *needle safe, int nlen,
		   const char *haystack safe, int hlen);
enum rxl_found rxl_advance(struct match_state *st safe, wint_t ch);
int rxl_info(struct match_state *st safe, int *lenp safe, int *totalp,
	      int *startp, int *since_startp);
int rxl_capture(struct match_state *st safe, int cap, int which,
		int *startp safe, int *lenp safe);
char *rxl_interp(struct match_state *s safe, const char *form safe);
void rxl_free_state(struct match_state *s);

/* These are 'or'ed in with the ch and reflect state *before*
 * the ch.  For state at EOF, use WEOF for the ch
 */
#define	RXL_SOD		(1 << 22)
#define	RXL_SOL		(1 << 23)
#define	RXL_SOW		(1 << 24)
#define	RXL_NOWBRK	(1 << 25) /* Not at a word boundary */
#define	RXL_POINT	(1 << 26)
#define	RXL_EOW		(1 << 27)
#define	RXL_EOL		(1 << 28)
#define	RXL_EOD		(1 << 29)
#define	RXL_ANCHOR	(1 << 30) /* Set RXLF_ANCHORED from there on */
#define	RXL_LAST RXL_ANCHOR
