/*
 * Copyright Neil Brown <neil@brown.name> 2015
 * May be distrubuted under terms of GPLv2 - see file:COPYING
 */
struct pane *tile_init(struct pane *display);
struct pane *tile_split(struct pane *p, int horiz, int after);
void tile_register(void);
int tile_grow(struct pane *p, int horiz, int size);
