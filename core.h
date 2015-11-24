/*
 * Copyright Neil Brown <neil@brown.name> 2015
 * May be distributed under terms of GPLv2 - see file:COPYING
 *
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
#include <limits.h>
#include <sys/stat.h>
#include "list.h"

#undef bool
typedef _Bool bool;

struct doc;
struct mark;
struct point;
struct attrset;
struct display;
struct pane;
struct command;
struct cmd_info;

#ifndef PRIVATE_DOC_REF
struct doc_ref {
	void	*p;
	int	i;
};
#endif

/* The 'editor' contains (by reference) everything else.
 * This captures and documents the global states, and allows
 * multiple "editors" in the one process, should that be valuable.
 *
 * Each document and each display contains a reference to the editor.
 * The root pane of a pane tree must have a display as the 'data', which allows
 * the editor to be found from any pane or point.
 */
struct display {
	struct editor		*ed;
	char			*mode, *next_mode;
	int			numeric, extra;
};

struct editor {
	struct event_base	*base;
	struct list_head	documents;
	struct doc		*docs;  /* document containing all documents */
	struct map		*commands;

	struct display		null_display;
};
struct pane *editor_new(void);
struct point *editor_choose_doc(struct editor *ed);
void doc_make_docs(struct editor *ed);
int editor_load_module(struct editor *ed, char *name);

struct doc {
	struct hlist_head	marks;
	struct tlist_head	points;
	struct docview {
		struct tlist_head head;
		struct command	  *notify;
		short		space;	/* extra space to allocate after a mark */
		short		marked;	/* being deleted */
	} *views;
	struct attrset		*attrs;
	int			nviews;
	struct editor		*ed;
	struct doc_operations	*ops;
	struct list_head	list;	/* ed->documents */
	struct map		*map;
	char			*name;
	char			*default_render;
	short			deleting; /* is begin destroyed */
};

struct doc_operations {
	void		(*replace)(struct point *pos, struct mark *end,
				   char *str, bool *first);
	int		(*reundo)(struct point *pos, bool undo);
	wint_t		(*step)(struct doc *d, struct mark *m, bool forward, bool move);
	void		(*set_ref)(struct doc *d, struct mark *m, bool start);
	int		(*same_ref)(struct doc *d, struct mark *a, struct mark *b);
	/* get/set attr operate on the attributes of the char immediately
	 * after the point/mark.  They fail at EOF.
	 */
	char		*(*get_attr)(struct doc *d, struct mark *m, bool forward, char *attr);
	int		(*set_attr)(struct point *pos, char *attr, char *val);
};

void doc_init(struct doc *d);
int doc_add_view(struct doc *d, struct command *c);
void doc_del_view(struct doc *d, struct command *c);
int doc_find_view(struct doc *d, struct command *c);
struct point *doc_new(struct editor *ed, char *type);
struct pane *doc_from_text(struct pane *parent, char *name, char *text);
struct pane *doc_open(struct editor *ed, struct pane *parent, int fd,
		      char *name, char *render);
void doc_set_name(struct doc *d, char *name);
struct doc *doc_find(struct editor *ed, char *name);
void doc_promote(struct doc *d);
int  doc_destroy(struct doc *d);

struct pane *render_attach(char *name, struct pane *parent);

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
	short			viewnum;
	short			rpos;	/* use by renderer to identify positions with
					 * a document object (which displays as more than
					 * a char
					 */
};

struct point {
	struct mark		m;
	struct doc		*doc;
	struct point		**owner;
	int			size;
	struct tlist_head	lists[];
};

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
void mark_reset(struct doc *d, struct mark *m);
void mark_forward_over(struct mark *m, struct mark *m2);
void mark_backward_over(struct mark *m, struct mark *mp);
void point_notify_change(struct point *p, struct mark *m);
void doc_notify_change(struct doc *d, struct mark *m);
void doc_check_consistent(struct doc *d);
void point_to_mark(struct point *p, struct mark *m);
void mark_to_mark(struct doc *d, struct mark *m, struct mark *target);
struct point *point_new(struct doc *d, struct point **owner);
struct point *point_dup(struct point *p, struct point **owner);
wint_t mark_next(struct doc *d, struct mark *m);
wint_t mark_prev(struct doc *d, struct mark *m);
struct mark *mark_at_point(struct point *p, int view);
void points_resize(struct doc *d);
void points_attach(struct doc *d, int view);
void point_free(struct point *p);
struct mark *vmark_next(struct mark *m);
struct mark *vmark_prev(struct mark *m);
struct mark *vmark_first(struct doc *d, int view);
struct mark *vmark_last(struct doc *d, int view);
struct mark *vmark_matching(struct doc *d, struct mark *m);
struct mark *vmark_at_point(struct point *pt, int view);

static inline int mark_ordered(struct mark *m1, struct mark *m2)
{
	return m1->seq < m2->seq;
}
static inline int mark_same(struct doc *d, struct mark *m1, struct mark *m2)
{
	return d->ops->same_ref(d, m1, m2);
}
static inline int mark_ordered_or_same(struct doc *d, struct mark *m1, struct mark *m2)
{
	return mark_ordered(m1, m2) || mark_same(d, m1, m2);
}
static inline struct attrset **mark_attr(struct mark *m)
{
	return &m->attrs;
}

/* Attributes */
void attr_free(struct attrset **setp);
char *attr_find(struct attrset *set, char *key);
int attr_del(struct attrset **setp, char *key);
int attr_set_str(struct attrset **setp, char *key, char *val, int keynum);
char *attr_get_str(struct attrset *setp, char *key, int keynum);
int attr_find_int(struct attrset *set, char *key);
int attr_set_int(struct attrset **setp, char *key, int val);
void attr_trim(struct attrset **setp, int nkey);
struct attrset *attr_copy_tail(struct attrset *set, int nkey);
struct attrset *attr_collect(struct attrset *set, int pos, int prefix);
void attr_free(struct attrset **setp);



/* Commands */
struct command {
	int	(*func)(struct cmd_info *ci);
};

#define CMD(_name) {_name ## _func }
#define DEF_CMD(_name) \
	static int _name ## _func(struct cmd_info *ci); \
	static struct command _name = CMD(_name);	\
	static int _name ## _func(struct cmd_info *ci)
#define REDEF_CMD(_name) \
	static int _name ## _func(struct cmd_info *ci)

#define	ARRAY_SIZE(ra) (sizeof(ra) / sizeof(ra[0]))

/* Each event (above) is accompanied by a cmd_info structure.
 * 'key' and 'home' are always present, others only if relevant.
 * Numeric is present for 'key' and 'move'.  INT_MAX/2 means no number was
 *   requested so is usually treated like '1'.  Negative numbers are quite
 *   possible.
 * x,y are present for mouse events
 * 'str' is inserted by 'replace' and sought by 'search'
 * 'mark' is moved by 'move' and 'replace' deletes between point and mark.
 */
struct cmd_info {
	char		*key;
	struct pane	*home, *focus;
	int		numeric, extra;
	int		x,y;		/* relative to focus */
	int		hx, hy;		/* x,y mapped to 'home' */
	char		*str, *str2;
	struct mark	*mark;
	struct point	**pointp;
	struct command	*comm;
};
#define	NO_NUMERIC	(INT_MAX/2)
#define	RPT_NUM(ci)	((ci)->numeric == NO_NUMERIC ? 1 : (ci)->numeric)

struct map *key_alloc(void);
void key_free(struct map *m);
int key_handle(struct cmd_info *ci);
int key_handle_focus(struct cmd_info *ci);
int key_handle_xy(struct cmd_info *ci);
int key_lookup(struct map *m, struct cmd_info *ci);
struct command *key_lookup_cmd(struct map *m, char *c);
void key_add(struct map *map, char *k, struct command *comm);
void key_add_range(struct map *map, char *first, char *last,
		   struct command *comm);
struct command *key_register_prefix(char *name);


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

	struct command		*handle;
	void			*data;
	struct point		*point;
	struct attrset		*attrs;
};


enum {
	DAMAGED_CHILD	= 1,
	DAMAGED_SIZE	= 2, /* these the each impose the next. */
	DAMAGED_CONTENT	= 4,
	DAMAGED_CURSOR	= 8,
};

struct pane *pane_register(struct pane *parent, int z,
			   struct command *handle, void *data,
			   struct list_head *here);
void pane_reparent(struct pane *p, struct pane *newparent);
void pane_subsume(struct pane *p, struct pane *parent);
void pane_close(struct pane *p);
int pane_clone(struct pane *from, struct pane *parent);
void pane_resize(struct pane *p, int x, int y, int w, int h);
void pane_check_size(struct pane *p);
void pane_refresh(struct pane *p);
void pane_focus(struct pane *p);
struct pane *pane_with_cursor(struct pane *p, int *ox, int *oy);
void pane_damaged(struct pane *p, int type);
struct pane *pane_to_root(struct pane *p, int *x, int *y, int *z,
			  int *w, int *h);
int pane_masked(struct pane *p, int x, int y, int z, int *w, int *h);
struct editor *pane2ed(struct pane *p);
void pane_set_mode(struct pane *p, char *mode, int transient);
void pane_set_numeric(struct pane *p, int numeric);
void pane_set_extra(struct pane *p, int extra);
struct pane *pane_attach(struct pane *p, char *type, struct point *pt, char *arg);
void pane_clear(struct pane *p, char *attrs);
void pane_text(struct pane *p, wchar_t ch, char *attrs, int x, int y);
char *pane_attr_get(struct pane *p, char *key);
static inline struct point **pane_point(struct pane *p)
{
	while (p && !p->point)
		p = p->parent;
	if (p)
		return &p->point;
	return NULL;
}

static inline struct pane *pane_child(struct pane *p)
{
	/* Find a child (if any) with z=0.  There should be
	 * at most one.
	 */
	struct pane *c;
	list_for_each_entry(c, &p->children, siblings)
		if (c->z == 0)
			return c;
	return NULL;
}
struct pane *pane_final_child(struct pane *p);

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
static inline int doc_load_file(struct doc *d, struct point *p,
				int fd, char *name)
{
	struct cmd_info ci = {0};
	ci.pointp = &p;
	ci.extra = fd;
	ci.str = name;
	ci.key = "doc:load-file";
	return key_lookup(d->map, &ci);
}
static inline char *doc_getstr(struct point *from, struct mark *to)
{
	struct cmd_info ci = {0};
	struct doc *d = from->doc;
	int ret;

	ci.key = "doc:get-str";
	ci.pointp = &from;
	ci.mark = to;
	ret = key_lookup(d->map, &ci);
	if (!ret)
		return NULL;
	return ci.str;
}

static inline char *doc_attr(struct doc *d, struct mark *m, bool forward, char *attr)
{
	return d->ops->get_attr(d, m, forward, attr);
}
