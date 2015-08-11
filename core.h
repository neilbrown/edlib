
/*
 * Core elements include:
 * documents
 * marks and points
 * attributes
 * displays
 * panes
 * keymaps
 * commands
 */

#include <wchar.h>
#include "list.h"

#undef bool
typedef _Bool bool;

struct doc;
struct mark;
struct point;
struct attrset;
struct display;
struct pane;
struct keymap;
struct command;
struct cmd_info;

#ifndef PRIVATE_DOC_REF
struct doc_ref {
	void	*p;
	int	i;
};
#endif
struct doc {
	struct hlist_head	marks;
	struct tlist_head	points;
	struct docview {
		struct tlist_head head;
		struct command	  *notify;
	} *views;
	struct attrset		*attrs;
	int			nviews;
	struct doc_operations	*ops;
};

struct doc_operations {
	void		(*replace)(struct point *pos, struct mark *end,
				   char *str, bool *first);
	int		(*load_file)(struct point *pos, int fd);
	int		(*reundo)(struct point *pos, bool undo);
	wint_t		(*step)(struct doc *d, struct mark *m, bool forward, bool move);
	char		*(*get_str)(struct doc *d, struct mark *from, struct mark *to);
	void		(*set_ref)(struct doc *d, struct mark *m, bool start);
	int		(*same_ref)(struct doc *d, struct mark *a, struct mark *b);
};

void doc_init(struct doc *d);
int doc_add_view(struct doc *d, struct command *c);
int doc_find_view(struct doc *d, struct command *c);
struct doc *doc_new(char *type);
void doc_register_type(char *type, struct doc *(*new)(void));

/* Points and Marks */

enum {
	MARK_POINT = -1,
	MARK_UNGROUPED = -2
};

enum {
	GRP_HEAD = 0, // tlist_head list head
	GRP_MARK = 1, // tlist_head in mark.view
	GRP_LIST = 2, // tlist_head in point.lists
};

struct mark {
	struct doc_ref		ref;
	struct hlist_node	all;
	struct tlist_head	view;
	struct attrset		*attrs;
	int			seq;
	int			viewnum;
};

struct point {
	struct mark		m;
	struct doc		*doc;
	struct pane		*owner;
	int			size;
	struct tlist_head	lists[];
};

struct mark *mark_of_point(struct point *p);
struct mark *mark_dup(struct mark *m, int notype);
void mark_free(struct mark *m);
struct mark *doc_new_mark(struct doc *d, int view);
struct mark *doc_first_mark_all(struct doc *d);
struct mark *doc_next_mark_all(struct doc *d, struct mark *m);
struct mark *doc_prev_mark_all(struct doc *d, struct mark *m);
struct mark *doc_first_mark(struct doc *d, int viewnum);
struct mark *doc_next_mark(struct doc *d, struct mark *m);
struct mark *doc_prev_mark(struct doc *d, struct mark *m);
void point_reset(struct point *p);
void mark_forward_over(struct mark *m, struct mark *m2);
void mark_backward_over(struct mark *m, struct mark *mp);
void point_notify_change(struct point *p);
void doc_check_consistent(struct doc *d);
void point_to_mark(struct point *p, struct mark *m);
/*??*/struct doc_ref point_ref(struct point *p);
struct point *point_new(struct doc *d, struct pane *owner);
wint_t mark_next(struct doc *d, struct mark *m);
wint_t mark_prev(struct doc *d, struct mark *m);
struct mark *mark_at_point(struct point *p, int view);
void points_resize(struct doc *d);
void points_attach(struct doc *d, int view);

static inline int mark_ordered(struct mark *m1, struct mark *m2)
{
	return m1->seq < m2->seq;
}
static inline int mark_same(struct doc *d, struct mark *m1, struct mark *m2)
{
	return d->ops->same_ref(d, m1, m2);
}
static inline struct attrset **mark_attr(struct mark *m)
{
	return &m->attrs;
}

/* Attributes */
void attr_free(struct attrset **setp);



/* Commands */
struct command {
	int	(*func)(struct command *comm, struct cmd_info *ci);
	char	*name;
};

#define	CMD(func, name) {func, name}
#define	DEF_CMD(comm, func, name) static struct command comm = CMD(func, name)
#define	ARRAY_SIZE(ra) (sizeof(ra) / sizeof(ra[0]))

/* Each event (above) is accompanied by a cmd_info structure.
 * 'key' and 'focus' are always present, others only if relevant.
 * Numeric is present for 'key' and 'move'.  INT_MAX/2 means no number was
 *   requested so is usually treated like '1'.  Negative numbers are quite
 *   possible.
 * x,y are present for mouse events
 * 'str' is inserted by 'replace' and sought by 'search'
 * 'mark' is moved by 'move' and 'replace' deletes between point and mark.
 */
struct cmd_info {
	wint_t		key;
	struct pane	*focus;
	int		numeric, extra;
	int		x,y;
	char		*str;
	struct mark	*mark;
	struct pane	*point_pane;
};
#define	NO_NUMERIC	(INT_MAX/2)
#define	RPT_NUM(ci)	((ci)->numeric == NO_NUMERIC ? 1 : (ci)->numeric)

struct map *key_alloc(void);
int key_handle(struct cmd_info *ci);
int key_handle_focus(struct cmd_info *ci);
int key_handle_xy(struct cmd_info *ci);
int key_add(struct map *map, wint_t k, struct command *comm);
int key_add_range(struct map *map, wint_t first, wint_t last,
		   struct command *comm);
struct command *key_register_mode(char *name, int *mode);


/* pane */
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
	struct command		*refresh;
	void			*data;
	struct point		*point;
};


/* Inlines */

static inline wint_t doc_following(struct doc *d, struct mark *m)
{
	return d->ops->step(d, m, 1, 0);
}
static inline wint_t doc_prior(struct doc *d, struct mark *m)
{
	return d->ops->step(d, m, 0, 0);
}
static inline void doc_replace(struct point *p, struct mark *m,
			       char *str, bool *first)
{
	p->doc->ops->replace(p, m, str, first);
}
static inline int doc_undo(struct point *p, bool redo)
{
	return p->doc->ops->reundo(p, redo);
}
static inline int doc_load_file(struct point *p, int fd)
{
	return p->doc->ops->load_file(p, fd);
}
static inline char *doc_getstr(struct doc *d, struct mark *from, struct mark *to)
{
	return d->ops->get_str(d, from, to);
}

