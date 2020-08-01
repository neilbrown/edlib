/*
 * Copyright Neil Brown ©2015-2020 <neil@brown.name>
 * May be distributed under terms of GPLv2 - see file:COPYING
 */
struct match_state;
unsigned short *rxl_parse(const char *patn safe, int *lenp, int nocase);
unsigned short *safe rxl_parse_verbatim(const char *patn safe, int nocase);
struct match_state *safe rxl_prepare(unsigned short *rxl safe, bool anchored);
int rxl_advance(struct match_state *st safe, wint_t ch, int flag);
void rxl_info(struct match_state *st safe, int *lenp safe, int *totalp,
	      int *startp, int *since_startp);
void rxl_free_state(struct match_state *s safe);

#define	RXL_SOL	1
#define	RXL_EOL	2
#define	RXL_SOW	4
#define	RXL_EOW	8
#define	RXL_NOWBRK 16 /* Not at a word boundary */
