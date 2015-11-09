/*
 * Copyright Neil Brown <neil@brown.name> 2015
 * May be distrubuted under terms of GPLv2 - see file:COPYING
 */
void count_calculate(struct doc *d, struct mark *start, struct mark *end);

struct pane *popup_register(struct pane *p, char *name, char *content, char *key);
void popup_init(void);

struct map *emacs_register(void);

struct pane *ncurses_init(struct editor *ed);
void ncurses_end(void);
void pane_clear(struct pane *p, int attr, int x, int y, int w, int h);
void pane_text(struct pane *p, wchar_t ch, int attr, int x, int y);
