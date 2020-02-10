/*
 * Copyright Neil Brown ©2015-2019 <neil@brown.name>
 * May be distributed under terms of GPLv2 - see file:COPYING
 */
struct match_state;
unsigned short *rxl_parse(const char *patn safe, int *lenp, int nocase);
unsigned short *safe rxl_parse_verbatim(const char *patn safe, int nocase);
struct match_state *safe rxl_prepare(unsigned short *rxl safe,
				     int anchored, int *lenp);
void rxl_free_state(struct match_state *s safe);
int rxl_advance(struct match_state *st safe, wint_t ch, int flag);

#define	RXL_SOL	1
#define RXL_EOL	2
#define	RXL_SOW	4
#define	RXL_EOW	8
