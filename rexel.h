/*
 * Copyright Neil Brown ©2015 <neil@brown.name>
 * May be distributed under terms of GPLv2 - see file:COPYING
 */
struct match_state;
unsigned short *rxl_parse(char *patn, int *lenp, int nocase);
unsigned short *rxl_parse_verbatim(char *patn, int nocase);
struct match_state *rxl_prepare(unsigned short *rxl);
void rxl_free_state(struct match_state *s);
int rxl_advance(struct match_state *st, wint_t ch, int flag, int restart);
