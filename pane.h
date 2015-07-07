#include <wchar.h>
#include <event.h>

struct pane;
struct map;
union event_info;
typedef int (*refresh_fn)(struct pane *p, int damage);
struct pane {
	struct pane		*parent;
	struct list_head	siblings;
	struct list_head	children;
	int			x,y,z;
	int			h,w;
	struct pane		*focus;
	int			cx, cy;	/* cursor position */

	int			damaged;

	struct map		*keymap;
	refresh_fn		refresh;
	void			*data;
};

enum {
	DAMAGED_CHILD	= 1,
	DAMAGED_CURSOR	= 2,
	DAMAGED_SIZE	= 4,
	DAMAGED_POSN	= 8,
	DAMAGED_CONTENT	= 16,
	DAMAGED_FORCE	= 32, // redraw pane an children even if nothing has changed
};

struct pane *pane_register(struct pane *parent, int z,
			   refresh_fn refresh, void *data,
			   struct list_head *here);
void pane_reparent(struct pane *p, struct pane *newparent, struct list_head *here);
void pane_free(struct pane *p);
void pane_text(struct pane *p, wchar_t ch, int attr, int x, int y);
void pane_clear(struct pane *p, int attr, int x, int y, int w, int h);
void pane_resize(struct pane *p, int x, int y, int w, int h);
void pane_refresh(struct pane *p);
void pane_focus(struct pane *p);
void pane_set_modifier(struct pane *p, int mod);
void pane_damaged(struct pane *p, int type);
struct pane *pane_to_root(struct pane *p, int *x, int *y, int *w, int *h);
int pane_masked(struct pane *p, int x, int y, int z, int *w, int *h);

struct pane *ncurses_init(struct event_base *base, struct map *keymap);
void ncurses_end(void);
