/*
 * Copyright Neil Brown <neil@brown.name> 2015
 * May be distrubuted under terms of GPLv2 - see file:COPYING
 */
#include <wchar.h>
#include <event.h>

struct pane;
struct map;

enum {
	DAMAGED_CHILD	= 1,
	DAMAGED_CURSOR	= 2,
	DAMAGED_SIZE	= 4,
	DAMAGED_POSN	= 8,
	DAMAGED_CONTENT	= 16,
	DAMAGED_FORCE	= 32, // redraw pane and children even if nothing has changed
};

struct pane *pane_register(struct pane *parent, int z,
			   struct command *refresh, void *data,
			   struct list_head *here);
void pane_reparent(struct pane *p, struct pane *newparent, struct list_head *here);
void pane_subsume(struct pane *p, struct pane *parent);
void pane_free(struct pane *p);
void pane_text(struct pane *p, wchar_t ch, int attr, int x, int y);
void pane_clear(struct pane *p, int attr, int x, int y, int w, int h);
void pane_close(struct pane *p);
int pane_clone(struct pane *from, struct pane *parent);
void pane_resize(struct pane *p, int x, int y, int w, int h);
void pane_check_size(struct pane *p);
void pane_refresh(struct pane *p);
void pane_focus(struct pane *p);
struct pane *pane_with_cursor(struct pane *p, int *ox, int *oy);
void pane_damaged(struct pane *p, int type);
struct pane *pane_to_root(struct pane *p, int *x, int *y, int *w, int *h);
int pane_masked(struct pane *p, int x, int y, int z, int *w, int *h);
void pane_set_mode(struct pane *p, char *mode, int transient);
void pane_set_numeric(struct pane *p, int numeric);
void pane_set_extra(struct pane *p, int extra);
struct editor *pane2ed(struct pane *p);

struct pane *ncurses_init(struct editor *ed);
void ncurses_end(void);
