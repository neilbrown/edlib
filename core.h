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
	struct mark		*point;
	struct attrset		*attrs;
};

struct command {
	int	(*func)(struct cmd_info *ci);
};

/* this is ->data for a document pane.  Only core-doc and
 * individual document handlers can know about this.
 */
struct doc_data {
	struct doc		*doc;
	struct command		notify;
	struct pane		*pane;
};

struct display {
	char			*mode, *next_mode;
	int			numeric, extra;
};

struct editor {
	struct pane		root;
	struct event_base	*base;
	struct doc		*docs;  /* document containing all documents */
	struct map		*commands;
};
struct pane *editor_new(void);
struct pane *editor_choose_doc(struct editor *ed);
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
	struct pane		*home; /* pane in null_display which owns this doc*/
	struct map		*map;
	char			*name;
	short			deleting; /* is begin destroyed */
};

void doc_init(struct doc *d);
struct doc *doc_new(struct editor *ed, char *type);
struct pane *doc_from_text(struct pane *parent, char *name, char *text);
struct pane *doc_open(struct editor *ed, int fd, char *name);
struct pane *doc_attach_view(struct pane *parent, struct pane *doc, char *render);
struct pane *doc_attach(struct pane *parent, struct doc *d);
void doc_set_name(struct doc *d, char *name);
struct pane *doc_find(struct editor *ed, char *name);
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

#ifndef MARK_DATA_PTR
#define MARK_DATA_PTR void
#endif
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
	MARK_DATA_PTR		*mdata;
};

/* A point uses this for the mdata */
struct point_links {
	int			size;
	struct mark		*pt;
	struct tlist_head	lists[];
};


struct mark *mark_dup(struct mark *m, int notype);
void mark_free(struct mark *m);
struct mark *doc_new_mark(struct doc *d, int view);
struct mark *doc_first_mark_all(struct doc *d);
struct mark *doc_next_mark_all(struct mark *m);
struct mark *doc_prev_mark_all(struct mark *m);
struct mark *doc_prev_mark_all_safe(struct doc *d, struct mark *m);
struct mark *doc_next_mark(struct mark *m);
struct mark *doc_prev_mark(struct mark *m);
void point_reset(struct mark *p);
void mark_reset(struct doc *d, struct mark *m);
void __mark_reset(struct doc *d, struct mark *m, int new, int end);
void mark_forward_over(struct mark *m, struct mark *m2);
void mark_backward_over(struct mark *m, struct mark *mp);
void point_notify_change(struct doc *d, struct mark *p, struct mark *m);
void doc_notify_change(struct doc *d, struct mark *m);
void doc_check_consistent(struct doc *d);
void point_to_mark(struct mark *p, struct mark *m);
void mark_to_mark(struct mark *m, struct mark *target);
int mark_same(struct doc *d, struct mark *m1, struct mark *m2);
int mark_same2(struct doc *d, struct mark *m1, struct mark *m2, struct cmd_info *ci);
int mark_same_pane(struct pane *p, struct mark *m1, struct mark *m2,
		   struct cmd_info *ci);
struct mark *point_new(struct doc *d);
struct mark *point_dup(struct mark *p);
wint_t mark_step(struct doc *d, struct mark *m, int forward, int move, struct cmd_info *ci);
wint_t mark_step2(struct doc *d, struct mark *m, int forward, int move);
wint_t mark_next(struct doc *d, struct mark *m);
wint_t mark_prev(struct doc *d, struct mark *m);
struct mark *mark_at_point(struct pane *p, struct mark *pm, int view);
struct mark *do_mark_at_point(struct doc *d, struct mark *pt, int view);
void points_resize(struct doc *d);
void points_attach(struct doc *d, int view);
struct mark *vmark_next(struct mark *m);
struct mark *vmark_prev(struct mark *m);
struct mark *do_vmark_first(struct doc *d, int view);
struct mark *do_vmark_last(struct doc *d, int view);
struct mark *vmark_matching(struct pane *p, struct mark *m);
struct mark *do_vmark_at_point(struct doc *d, struct mark *pt, int view);
struct mark *vmark_first(struct pane *p, int view);
struct mark *vmark_last(struct pane *p, int view);
struct mark *vmark_at_point(struct pane *p, int view);

static inline int mark_ordered(struct mark *m1, struct mark *m2)
{
	return m1->seq < m2->seq;
}
static inline int mark_ordered_or_same(struct doc *d, struct mark *m1, struct mark *m2)
{
	return mark_ordered(m1, m2) || mark_same(d, m1, m2);
}

static inline int mark_ordered_or_same_pane(struct pane *p, struct mark *m1, struct mark *m2)
{
	return mark_ordered(m1, m2) || mark_same_pane(p, m1, m2, NULL);
}

static inline int mark_ordered_not_same(struct doc *d, struct mark *m1, struct mark *m2)
{
	return mark_ordered(m1, m2) && !mark_same(d, m1, m2);
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
struct lookup_cmd {
	struct command	c;
	struct map	**m;
};

#define CMD(_name) {_name ## _func }
#define DEF_CMD(_name) \
	static int _name ## _func(struct cmd_info *ci); \
	static struct command _name = CMD(_name);	\
	static int _name ## _func(struct cmd_info *ci)
#define REDEF_CMD(_name) \
	static int _name ## _func(struct cmd_info *ci)

#define DEF_LOOKUP_CMD(_name, _map) \
	static struct lookup_cmd _name = { { key_lookup_cmd_func }, &_map };

int key_lookup_cmd_func(struct cmd_info *ci);

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
	struct mark	*mark, *mark2;
	struct command	*comm, *comm2;
	void		*misc;		/* command specific */
};
#define	NO_NUMERIC	(INT_MAX/2)
#define	RPT_NUM(ci)	((ci)->numeric == NO_NUMERIC ? 1 : (ci)->numeric)

struct map *key_alloc(void);
void key_free(struct map *m);
int key_handle(struct cmd_info *ci);
int key_handle_focus(struct cmd_info *ci);
int key_handle_xy(struct cmd_info *ci);
int key_handle_focus_point(struct cmd_info *ci);
int key_handle_xy_point(struct cmd_info *ci);
int key_lookup(struct map *m, struct cmd_info *ci);
struct command *key_lookup_cmd(struct map *m, char *c);
void key_add(struct map *map, char *k, struct command *comm);
void key_add_range(struct map *map, char *first, char *last,
		   struct command *comm);
struct command *key_register_prefix(char *name);

enum {
	DAMAGED_CHILD	= 1,
	DAMAGED_SIZE	= 2, /* these the each impose the next. */
	DAMAGED_CONTENT	= 4,
	DAMAGED_CURSOR	= 8,
};

struct pane *pane_register(struct pane *parent, int z,
			   struct command *handle, void *data,
			   struct list_head *here);
void pane_init(struct pane *p, struct pane *par, struct list_head *here);
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
struct pane *pane_attach(struct pane *p, char *type, struct pane *dp, char *arg);
void pane_clear(struct pane *p, char *attrs);
void pane_text(struct pane *p, wchar_t ch, char *attrs, int x, int y);
char *pane_attr_get(struct pane *p, char *key);

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
	return mark_step2(d, m, 1, 0);
}
static inline wint_t doc_prior(struct doc *d, struct mark *m)
{
	return mark_step2(d, m, 0, 0);
}
static inline void doc_replace(struct pane *p, struct mark *m,
			       char *str, bool *first)
{
	struct cmd_info ci = {0};
	ci.key = "doc:replace";
	ci.focus = p;
	ci.mark = m;
	ci.str = str;
	ci.extra = *first;
	ci.numeric = 1;
	key_handle_focus(&ci);
	*first = ci.extra;
}
static inline int doc_undo(struct pane *p, bool redo)
{
	struct cmd_info ci = {0};
	ci.focus = p;
	ci.numeric = redo ? 1 : 0;
	ci.key = "doc:reundo";
	return key_handle_focus(&ci);
}
static inline int doc_load_file(struct pane *p, int fd, char *name)
{
	struct cmd_info ci = {0};
	ci.focus = p;
	ci.extra = fd;
	ci.str = name;
	ci.key = "doc:load-file";
	return key_handle_focus(&ci);
}
static inline char *doc_getstr(struct pane *from, struct mark *to)
{
	struct cmd_info ci = {0};
	int ret;

	ci.key = "doc:get-str";
	ci.focus = from;
	ci.mark = to;
	ret = key_handle_focus(&ci);
	if (!ret)
		return NULL;
	return ci.str;
}

static inline char *doc_attr(struct pane *dp, struct mark *m, bool forward, char *attr)
{
	struct cmd_info ci = {0};

	ci.key = "doc:get-attr";
	ci.focus = dp;
	ci.mark = m;
	ci.numeric = forward ? 1 : 0;
	ci.str = attr;
	if (key_handle_focus(&ci) == 0)
		return NULL;
	return ci.str2;
}

static inline int doc_set_attr(struct pane *p, struct mark *pt,
			       char *attr, char *val)
{
	struct cmd_info ci = {0};

	ci.key = "doc:set-attr";
	ci.focus = p;
	ci.mark = pt;
	ci.str = attr;
	ci.str2 = val;
	return key_handle_focus(&ci);
}


static inline int doc_add_view(struct pane *p, struct command *c, int size)
{
	struct cmd_info ci = {0};
	ci.focus = p;
	ci.key = "doc:add-view";
	ci.comm2 = c;
	ci.extra = size;
	if (key_handle_focus(&ci) == 0)
		return -1;
	return ci.extra;
}

static inline void doc_del_view(struct pane *p, struct command *c)
{
	struct cmd_info ci = {0};
	ci.focus = p;
	ci.key = "doc:del-view";
	ci.comm2 = c;
	key_handle_focus(&ci);
}

static inline int doc_find_view(struct pane *p, struct command *c)
{
	struct cmd_info ci = {0};
	ci.focus = p;
	ci.key = "doc:find-view";
	ci.comm2 = c;
	if (key_handle_focus(&ci) == 0)
		return -1;
	return ci.extra;
}

static inline struct doc *doc_from_pane(struct pane *p)
{
	struct cmd_info ci = {0};
	ci.focus = p;
	ci.key = "doc:find";
	if (key_handle_focus(&ci) == 0)
		return NULL;
	return ci.misc;
}

