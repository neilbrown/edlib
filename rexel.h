/*
 * Copyright Neil Brown ©2015-2020 <neil@brown.name>
 * May be distributed under terms of GPLv2 - see file:COPYING
 */
struct match_state;
enum rxl_found {
	RXL_NOMATCH,	/* No match has been found at all */
	RXL_CONTINUE,	/* No match here, but it is worth looking further */
	RXL_DONE,	/* A match was previously reported, but no further match
			 * can be found was we are anchored on that match.
			 */
	RXL_MATCH,	/* There was a match once the char was processed */
	RXL_MATCH_FLAG,	/* A match was found due to flags, but not once char was
			 * processed.
			 */
};
unsigned short *rxl_parse(const char *patn safe, int *lenp, int nocase);
unsigned short *safe rxl_parse_verbatim(const char *patn safe, int nocase);
struct match_state *safe rxl_prepare(unsigned short *rxl safe, bool anchored);
enum rxl_found rxl_advance(struct match_state *st safe, wint_t ch);
void rxl_info(struct match_state *st safe, int *lenp safe, int *totalp,
	      int *startp, int *since_startp);
void rxl_free_state(struct match_state *s safe);

/* These are 'or'ed in with the ch and reflect state *before*
 * the ch.  For state at EOF, use WEOF for the ch
 */
#define	RXL_SOL	(1 << 22)
#define	RXL_SOW	(1 << 23)
#define	RXL_NOWBRK (1 << 24) /* Not at a word boundary */
#define	RXL_EOW	(1 << 25)
#define	RXL_EOL	(1 << 26)
