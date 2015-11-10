/*
 * Copyright Neil Brown <neil@brown.name> 2015
 * May be distrubuted under terms of GPLv2 - see file:COPYING
 */

struct pane *ncurses_init(struct editor *ed);
void pane_clear(struct pane *p, int attr, int x, int y, int w, int h);
void pane_text(struct pane *p, wchar_t ch, int attr, int x, int y);
