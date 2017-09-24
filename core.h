/*
 * Copyright Neil Brown ©2015 <neil@brown.name>
 * May be distributed under terms of GPLv2 - see file:COPYING
 *
 * Core elements include:
 * documents
 * marks and points
 * attributes
 * panes
 * keymaps
 * commands
 */

#include <wchar.h>
#include <limits.h>
#include <sys/stat.h>

#include "safe.h"

#include "list.h"

#undef bool
typedef _Bool bool;

struct doc;
struct mark;
struct attrset;
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
 * Each document and contains a reference to the editor which is the root of the
 * pane tree.
 */
struct pane {
	struct pane		*parent;
	struct list_head	siblings;
	struct list_head	children;
	int			x,y,z;
	int			h,w;
	struct pane		*focus;
	int			cx, cy;	/* cursor position */
	int			abs_z, abs_zhi; /* 'hi' is the max of all children */

	int			damaged;

	struct command		*handle;
	void			*data safe;
	struct mark		*pointer;
	struct attrset		*attrs;
	struct list_head	notifiers, notifiees;
};

struct command {
	int	(*func)(const struct cmd_info *ci safe);
	int	refcnt; /* only if 'free' is not NULL */
	void	(*free)(struct command *c safe);
};

static inline struct command *safe command_get(struct command * safe c)
{
	if (!(void*) c)
		return c;
	if (c->free)
		c->refcnt += 1;
	return c;
}

static inline void command_put(struct command *c)
{
	if (c && c->free && c->refcnt-- == 1)
		c->free(c);
}

struct notifier {
	struct pane		*notifiee safe;
	char			*notification safe;
	struct list_head	notifier_link, notifiee_link;
	int			noted;
};
void pane_add_notify(struct pane *target safe, struct pane *source safe, char *msg safe);
int pane_notify(struct pane *p safe, char *notification safe, struct mark *m, struct mark *m2,
		char *str, char *str2, int num, int num2, struct command *comm2);
void pane_drop_notifiers(struct pane *p safe, char *notification);

void editor_delayed_free(struct pane *ed safe, struct pane *p safe);
struct pane *editor_new(void);
void *memsave(struct pane *p safe, char *buf, int len);
char *strsave(struct pane *p safe, char *buf);

/* This is declared here so sparse knows it is global */
void edlib_init(struct pane *ed safe);

struct doc {
	struct hlist_head	marks;
	struct tlist_head	points;
	struct docview {
		struct tlist_head head;
		short		  state;	/* 0 = unused, 1 = active, 2 = being deleted */
	} /* safe iff nviews > 0 */ *views;
	int			nviews;
	struct pane		*home safe; /* pane which owns this doc*/
	char			*name;
	bool			autoclose;
	bool			filter;
};

void doc_init(struct doc *d safe);
void doc_free(struct doc *d safe);
struct pane *doc_new(struct pane *p safe, char *type, struct pane *parent);
struct pane *doc_attach_view(struct pane *parent safe, struct pane *doc safe, char *render);
extern struct map *doc_default_cmd safe;
void doc_setup(struct pane *ed safe);

#define CHAR_RET(_c) ((_c & 0xFFFFF) | 0x100000)

#define is_eol(c) ({int __c = c; __c == '\n' || c == '\v'; })

struct pane *render_attach(char *name, struct pane *parent safe);

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
	unsigned short		rpos;	/* Used by renderer to identify
					 * positions within a document object
					 * (which displays as more than one
					 * char).
					 */
	MARK_DATA_PTR		*mdata;
	void			*mtype;	/* can be used to validate type of mdata */
	void			(*refcnt)(struct mark *m safe, int cnt);
};

#define NEVER_RPOS (USHRT_MAX)
#define NO_RPOS (USHRT_MAX-1)

/* A point uses this for the mdata */
struct point_links {
	int			size;
	struct mark		*pt safe;
	struct tlist_head	lists[];
};

struct mark *safe mark_dup(struct mark *m safe, int notype);
void mark_free(struct mark *m);
struct mark *doc_new_mark(struct doc *d safe, int view);
struct mark *doc_first_mark_all(struct doc *d safe);
struct mark *doc_next_mark_all(struct mark *m safe);
struct mark *doc_prev_mark_all(struct mark *m safe);
struct mark *doc_next_mark_view(struct mark *m safe);
struct mark *doc_prev_mark_view(struct mark *m safe);
void point_reset(struct mark *p);
void mark_reset(struct doc *d safe, struct mark *m safe, int end);
void mark_to_end(struct doc *d safe, struct mark *m safe, int end);
void mark_forward_over(struct mark *m safe, struct mark *m2 safe);
void mark_backward_over(struct mark *m safe, struct mark *mp safe);
void doc_notify_change(struct doc *d safe, struct mark *m, struct mark *m2);
void doc_check_consistent(struct doc *d safe);
void point_to_mark(struct mark *p safe, struct mark *m safe);
void mark_to_mark(struct mark *m safe, struct mark *target safe);
int mark_same_pane(struct pane *p safe, struct mark *m1 safe, struct mark *m2 safe);
struct mark *safe point_new(struct doc *d safe);
struct mark *safe point_dup(struct mark *p safe);
wint_t mark_step(struct doc *d safe, struct mark *m safe, int forward, int move);
wint_t mark_step2(struct doc *d safe, struct mark *m safe, int forward, int move);
wint_t mark_step_pane(struct pane *p safe, struct mark *m safe, int forward, int move);

wint_t mark_next(struct doc *d safe, struct mark *m safe);
wint_t mark_prev(struct doc *d safe, struct mark *m safe);
wint_t mark_next_pane(struct pane *p safe, struct mark *m safe);
wint_t mark_prev_pane(struct pane *p safe, struct mark *m safe);
struct mark *mark_at_point(struct pane *p safe, struct mark *pm, int view);
struct mark *do_mark_at_point(struct mark *pt safe, int view);
void points_resize(struct doc *d safe);
void points_attach(struct doc *d safe, int view);
struct mark *vmark_next(struct mark *m safe);
struct mark *vmark_prev(struct mark *m safe);
struct mark *do_vmark_first(struct doc *d safe, int view);
struct mark *do_vmark_last(struct doc *d safe, int view);
struct mark *vmark_matching(struct pane *p safe, struct mark *m safe);
struct mark *do_vmark_at_point(struct pane *p safe, struct doc *d safe, struct mark *pt safe, int view);
struct mark *do_vmark_at_or_before(struct pane *p safe, struct doc *d safe, struct mark *m safe, int view);
struct mark *vmark_first(struct pane *p safe, int view);
struct mark *vmark_last(struct pane *p safe, int view);
struct mark *vmark_at_point(struct pane *p safe, int view);
struct mark *vmark_at_or_before(struct pane *p safe, struct mark *m safe, int view);
struct mark *vmark_new(struct pane *p safe, int view);

static inline int mark_ordered(struct mark *m1 safe, struct mark *m2 safe)
{
	return m1->seq < m2->seq;
}

static inline int mark_ordered_or_same_pane(struct pane *p safe,
						    struct mark *m1 safe, struct mark *m2 safe)
{
	return mark_ordered(m1, m2) || mark_same_pane(p, m1, m2);
}

static inline int mark_ordered_not_same_pane(struct pane *p safe, struct mark *m1 safe,
					     struct mark *m2 safe)
{
	return mark_ordered(m1, m2) && !mark_same_pane(p, m1, m2);
}

static inline struct attrset **safe mark_attr(struct mark *m safe)
{
	return &m->attrs;
}

/* Attributes */
char *attr_find(struct attrset *set, char *key safe);
int attr_del(struct attrset **setp safe, char *key safe);
int attr_set_str(struct attrset **setp safe, char *key safe, char *val);
int attr_set_str_key(struct attrset **setp safe, char *key safe, char *val, int keynum);
char *attr_get_str(struct attrset *setp, char *key safe, int keynum);
char *attr_get_next_key(struct attrset *set, char *key safe, int keynum,
			char **valp safe);
int attr_find_int(struct attrset *set, char *key safe);
int attr_set_int(struct attrset **setp safe, char *key safe, int val);
void attr_trim(struct attrset **setp safe, int nkey);
struct attrset *attr_copy_tail(struct attrset *set, int nkey);
struct attrset *attr_collect(struct attrset *set, unsigned int pos, int prefix);
void attr_free(struct attrset **setp safe);

/* Commands */
struct lookup_cmd {
	struct command	c;
	struct map	**m safe;
	struct map	**dflt;
};

#define CMD(_name) {_name ## _func , 0, NULL}
#define DEF_CMD(_name) \
	static int _name ## _func(const struct cmd_info *ci safe); \
	static struct command _name = CMD(_name);	\
	static int _name ## _func(const struct cmd_info *ci safe)
#define REDEF_CMD(_name) \
	static int _name ## _func(const struct cmd_info *ci safe)

#define DEF_LOOKUP_CMD(_name, _map) \
	static struct lookup_cmd _name = { { key_lookup_cmd_func, 0, NULL }, &_map, NULL };
#define DEF_LOOKUP_CMD_DFLT(_name, _map, _dflt)				\
	static struct lookup_cmd _name = { { key_lookup_cmd_func, 0, NULL}, &_map, &_dflt };

int key_lookup_cmd_func(const struct cmd_info *ci safe);

#define	ARRAY_SIZE(ra) (sizeof(ra) / sizeof(ra[0]))

/* Each event (above) is accompanied by a cmd_info structure.
 * 'key' and 'home' are always present, others only if relevant.
 * Num is present for 'key' and 'move'.  INT_MAX/2 means no number was
 *   requested so is usually treated like '1'.  Negative numbers are quite
 *   possible.
 * x,y are present for mouse events
 * 'str' is inserted by 'replace' and sought by 'search'
 * 'mark' is moved by 'move' and 'replace' deletes between point and mark.
 */
struct cmd_info {
	char		*key safe;
	struct pane	*home safe, *focus safe;
	int		num, num2;
	int		x,y;		/* relative to focus */
	char		*str, *str2;
	struct mark	*mark, *mark2;
	struct command	*comm safe;
	struct command	*comm2;
};

#define	NO_NUMERIC	(INT_MAX/2)
#define	RPT_NUM(ci)	((ci)->num == NO_NUMERIC ? 1 : (ci)->num == -NO_NUMERIC ? -1 : (ci)->num)

struct map *safe key_alloc(void);
void key_free(struct map *m safe);
int key_handle(const struct cmd_info *ci safe);
int key_lookup(struct map *m safe, const struct cmd_info *ci safe);
int key_lookup_prefix(struct map *m safe, const struct cmd_info *ci safe);
struct command *key_lookup_cmd(struct map *m safe, char *c safe);
void key_add(struct map *map safe, char *k safe, struct command *comm);
void key_add_range(struct map *map safe, char *first safe, char *last safe,
		   struct command *comm);
struct command *key_register_prefix(char *name safe);

/* DAMAGED_CURSOR, and DAMAGED_SIZE propagate down.
 * If any flag is set on children, DAMAGED_CHILD is set.
 */
enum {
	DAMAGED_CHILD	= 1,
	DAMAGED_VIEW	= 128,
	DAMAGED_SIZE	= 2, /* these three each impose the next. */
	DAMAGED_CONTENT	= 4,
	DAMAGED_CURSOR	= 8,

	DAMAGED_SIZE_CHILD = 16, /* a child has changed size */
	DAMAGED_VIEW_CHILD = 32, /* a child needs to adjust the view */

	DAMAGED_POSTORDER= 512,
	DAMAGED_CLOSED	= 1024,
};
#define DAMAGED_NEED_CALL (DAMAGED_SIZE | DAMAGED_CONTENT | DAMAGED_CURSOR)

struct xy {int x,y;};
struct pane *safe pane_register(struct pane *parent, int z,
				struct command *handle safe, void *data,
				struct list_head *here);
void pane_init(struct pane *p safe, struct pane *par, struct list_head *here);
void pane_reparent(struct pane *p safe, struct pane *newparent safe);
void pane_subsume(struct pane *p safe, struct pane *parent safe);
void pane_close(struct pane *p safe);
void pane_notify_close(struct pane *p safe);
void pane_resize(struct pane *p safe, int x, int y, int w, int h);
void pane_refresh(struct pane *p safe, struct mark *pointer);
void pane_focus(struct pane *p);
void pane_damaged(struct pane *p, int type);
void pane_clone_children(struct pane *from, struct pane *to);
struct pane *pane_my_child(struct pane *p, struct pane *c);

int pane_masked(struct pane *p safe, int x, int y, int abs_z, int *w, int *h);
char *pane_attr_get(struct pane *p, char *key safe);
char *pane_mark_attr(struct pane *p safe, struct mark *m safe, bool forward, char *key safe);
void pane_absxy(struct pane *p, int *x safe, int *y safe, int *w safe, int *h safe);
void pane_relxy(struct pane *p, int *x safe, int *y safe);
void pane_map_xy(struct pane *orig, struct pane *target, int *x safe, int *y safe);
struct pane *do_call_pane(char *key safe, struct pane *focus safe,
			  int num, struct mark *m, char *str,
			  int num2, struct mark *m2, char *str2,
			  int x, int y);
struct mark *do_call_mark(char *key safe, struct pane *focus safe,
			  int num, struct mark *m, char *str,
			  int num2, struct mark *m2, char *str2,
			  int x, int y);
struct mark *do_call_mark2(char *key safe, struct pane *focus safe,
			   int num, struct mark *m, char *str,
			   int num2, struct mark *m2, char *str2,
			   int x, int y);
char *do_call_str(char *key safe, struct pane *focus safe,
		  int num, struct mark *m, char *str,
		  int num2, struct mark *m2, char *str2,
		  int x, int y);
struct xy pane_scale(struct pane *p safe);

static inline int pane_attr_get_int(struct pane *p safe, char *key safe)
{
	char *c = pane_attr_get(p, key);
	int rv;
	char *end;
	if (!c)
		return -1;
	rv = strtol(c, &end, 10);
	if (end == c || !end || *end)
		return -1;
	return rv;
}

/* Inlines */

static inline wint_t doc_following_pane(struct pane *p safe, struct mark *m safe)
{
	return mark_step_pane(p, m, 1, 0);
}
static inline wint_t doc_prior_pane(struct pane *p safe, struct mark *m safe)
{
	return mark_step_pane(p, m, 0, 0);
}
static inline int doc_undo(struct pane *p safe, bool redo)
{
	struct cmd_info ci = {.key = "doc:reundo", .focus=p, .home=p, .comm = safe_cast 0 };

	ci.num = redo ? 1 : 0;
	return key_handle(&ci);
}

static inline int doc_set_attr(struct pane *p safe, struct mark *pt safe,
			       char *attr safe, char *val)
{
	struct cmd_info ci = {.key = "doc:set-attr", .focus = p, .home = p, .comm = safe_cast 0};

	ci.mark = pt;
	ci.str = attr;
	ci.str2 = val;
	return key_handle(&ci);
}

static inline int doc_add_view(struct pane *p safe)
{
	struct cmd_info ci = {.key = "doc:add-view", .focus = p, .home = p, .comm = safe_cast 0};
	int ret;

	ret = key_handle(&ci);
	if (ret <= 0)
		return -1;
	return ret - 1;
}

static inline void doc_del_view(struct pane *p safe, int num)
{
	struct cmd_info ci = {.key = "doc:del-view", .focus = p, .home = p, .comm = safe_cast 0};

	ci.num = num;
	key_handle(&ci);
}

/* Macro magic taken from
 * https://stackoverflow.com/questions/11761703/overloading-macro-on-number-of-arguments
 */
// get number of arguments with __NARG__
#define __NARG__(...)  __NARG_I_(__VA_ARGS__,__RSEQ_N())
#define __NARG_I_(...) __ARG_N(__VA_ARGS__)
#define __ARG_N( \
      _1, _2, _3, _4, _5, _6, _7, _8, _9,_10, \
     _11,_12,_13,_14,_15,_16,_17,_18,_19,_20, \
     _21,_22,_23,_24,_25,_26,_27,_28,_29,_30, \
     _31,_32,_33,_34,_35,_36,_37,_38,_39,_40, \
     _41,_42,_43,_44,_45,_46,_47,_48,_49,_50, \
     _51,_52,_53,_54,_55,_56,_57,_58,_59,_60, \
     _61,_62,_63,N,...) N
#define __RSEQ_N() \
     63,62,61,60,                   \
     59,58,57,56,55,54,53,52,51,50, \
     49,48,47,46,45,44,43,42,41,40, \
     39,38,37,36,35,34,33,32,31,30, \
     29,28,27,26,25,24,23,22,21,20, \
     19,18,17,16,15,14,13,12,11,10, \
     9,8,7,6,5,4,3,2,1,0

// general definition for any function name
#define _VFUNC_(name, n) name##n
#define _VFUNC(name, n) _VFUNC_(name, n)
#define VFUNC(func, ...) _VFUNC(func, __NARG__(__VA_ARGS__)) (__VA_ARGS__)

#define call(...) VFUNC(call, __VA_ARGS__)
#define call12(key, focus, num, mark, str, num2, mark2, str2, comm2, x, y, home) \
	do_call(key, focus, num, mark, str, num2, mark2, str2, comm2, x, y, home)
#define call11(key, focus, num, mark, str, num2, mark2, str2, comm2, x, y) \
	do_call(key, focus, num, mark, str, num2, mark2, str2, comm2, x, y, NULL)
#define call9(key, focus, num, mark, str, num2, mark2, str2, comm2) \
	do_call(key, focus, num, mark, str, num2, mark2, str2, comm2, 0, 0, NULL)
#define call8(key, focus, num, mark, str, num2, mark2, str2) \
	do_call(key, focus, num, mark, str, num2, mark2, str2, NULL, 0, 0, NULL)
#define call7(key, focus, num, mark, str, num2, mark2) \
	do_call(key, focus, num, mark, str, num2, mark2, NULL, NULL, 0, 0, NULL)
#define call6(key, focus, num, mark, str, num2) \
	do_call(key, focus, num, mark, str, num2, NULL, NULL, NULL, 0, 0, NULL)
#define call5(key, focus, num, mark, str) \
	do_call(key, focus, num, mark, str, 0, NULL, NULL, NULL, 0, 0, NULL)
#define call4(key, focus, num, mark) \
	do_call(key, focus, num, mark, NULL, 0, NULL, NULL, NULL, 0, 0, NULL)
#define call3(key, focus, num) \
	do_call(key, focus, num, NULL, NULL, 0, NULL, NULL, NULL, 0, 0, NULL)
#define call2(key, focus) \
	do_call(key, focus, 0, NULL, NULL, 0, NULL, NULL, NULL, 0, 0, NULL)

/* Often the 'home' arg is not need and defaults to 'focus', so it is at the
 * end of 'call'.  Often enough it has an important place, so have a set of
 * macros with 'home' first
 */
#define home_call(...) VFUNC(home_call, __VA_ARGS__)
#define home_call12(home, key, focus, num, mark, str, num2, mark2, str2, comm2, x, y) \
	do_call(key, focus, num, mark, str, num2, mark2, str2, comm2, x, y, home)
#define home_call10(home, key, focus, num, mark, str, num2, mark2, str2, comm2) \
	do_call(key, focus, num, mark, str, num2, mark2, str2, comm2, 0, 0, home)
#define home_call9(home, key, focus, num, mark, str, num2, mark2, str2) \
	do_call(key, focus, num, mark, str, num2, mark2, str2, NULL, 0, 0, home)
#define home_call8(home, key, focus, num, mark, str, num2, mark2) \
	do_call(key, focus, num, mark, str, num2, mark2, NULL, NULL, 0, 0, home)
#define home_call7(home, key, focus, num, mark, str, num2) \
	do_call(key, focus, num, mark, str, num2, NULL, NULL, NULL, 0, 0, home)
#define home_call6(home, key, focus, num, mark, str) \
	do_call(key, focus, num, mark, str, 0, NULL, NULL, NULL, 0, 0, home)
#define home_call5(home, key, focus, num, mark) \
	do_call(key, focus, num, mark, NULL, 0, NULL, NULL, NULL, 0, 0, home)
#define home_call4(home, key, focus, num) \
	do_call(key, focus, num, NULL, NULL, 0, NULL, NULL, NULL, 0, 0, home)
#define home_call3(home, key, focus) \
	do_call(key, focus, 0, NULL, NULL, 0, NULL, NULL, NULL, 0, 0, home)

/* comm2 is a bit like 'home' - often irrelevant, sometime important.
 * The *last* arg is always comm2 (unlike first for home in home_call)
 */
#define call_comm(...) VFUNC(call_comm, __VA_ARGS__)
#define call_comm11(key, focus, num, mark, str, num2, mark2, str2, x, y, comm2) \
	do_call(key, focus, num, mark, str, num2, mark2, str2, comm2, x, y, NULL)
#define call_comm9(key, focus, num, mark, str, num2, mark2, str2, comm2) \
	do_call(key, focus, num, mark, str, num2, mark2, str2, comm2, 0, 0, NULL)
#define call_comm8(key, focus, num, mark, str, num2, mark2, comm2) \
	do_call(key, focus, num, mark, str, num2, mark2, NULL, comm2, 0, 0, NULL)
#define call_comm7(key, focus, num, mark, str, num2, comm2) \
	do_call(key, focus, num, mark, str, num2, NULL, NULL, comm2, 0, 0, NULL)
#define call_comm6(key, focus, num, mark, str, comm2) \
	do_call(key, focus, num, mark, str, 0, NULL, NULL, comm2, 0, 0, NULL)
#define call_comm5(key, focus, num, mark, comm2) \
	do_call(key, focus, num, mark, NULL, 0, NULL, NULL, comm2, 0, 0, NULL)
#define call_comm4(key, focus, num, comm2) \
	do_call(key, focus, num, NULL, NULL, 0, NULL, NULL, comm2, 0, 0, NULL)
#define call_comm3(key, focus, comm2) \
	do_call(key, focus, 0, NULL, NULL, 0, NULL, NULL, comm2, 0, 0, NULL)

static inline int do_call(char *key safe, struct pane *focus safe, int num, struct mark *m,
			  char *str, int num2, struct mark *m2, char *str2,
			  struct command *comm2, int x, int y, struct pane *home)
{
	struct cmd_info ci = {.key = key, .focus = focus, .home = home ?: focus,
			      .num = num, .mark = m, .str = str,
			      .num2 = num2, .mark2 = m2, .str2 = str2,
			      .comm2 = comm2, .x = x, .y = y,
			      .comm = safe_cast 0};

	return key_handle(&ci);
}

/* comm_call() is quite different to call*(). It calls a specific command,
 * not a stack of panes.  The comm comes first.
 * This is mostly used for callback.
 */
#define comm_call(...) VFUNC(comm_call, __VA_ARGS__)
#define comm_call13(comm, key, focus, num, mark, str, num2, mark2, str2, comm2, x, y, home) \
	do_comm_call(comm, key, focus, num, mark, str, num2, mark2, str2, comm2, x, y, home)
#define comm_call12(comm, key, focus, num, mark, str, num2, mark2, str2, comm2, x, y) \
	do_comm_call(comm, key, focus, num, mark, str, num2, mark2, str2, comm2, x, y, NULL)
#define comm_call10(comm, key, focus, num, mark, str, num2, mark2, str2, comm2) \
	do_comm_call(comm, key, focus, num, mark, str, num2, mark2, str2, comm2, 0, 0, NULL)
#define comm_call9(comm, key, focus, num, mark, str, num2, mark2, str2) \
	do_comm_call(comm, key, focus, num, mark, str, num2, mark2, str2, NULL, 0, 0, NULL)
#define comm_call8(comm, key, focus, num, mark, str, num2, mark2) \
	do_comm_call(comm, key, focus, num, mark, str, num2, mark2, NULL, NULL, 0, 0, NULL)
#define comm_call7(comm, key, focus, num, mark, str, num2) \
	do_comm_call(comm, key, focus, num, mark, str, num2, NULL, NULL, NULL, 0, 0, NULL)
#define comm_call6(comm, key, focus, num, mark, str) \
	do_comm_call(comm, key, focus, num, mark, str, 0, NULL, NULL, NULL, 0, 0, NULL)
#define comm_call5(comm, key, focus, num, mark) \
	do_comm_call(comm, key, focus, num, mark, NULL, 0, NULL, NULL, NULL, 0, 0, NULL)
#define comm_call4(comm, key, focus, num) \
	do_comm_call(comm, key, focus, num, NULL, NULL, 0, NULL, NULL, NULL, 0, 0, NULL)
#define comm_call3(comm, key, focus) \
	do_comm_call(comm, key, focus, 0, NULL, NULL, 0, NULL, NULL, NULL, 0, 0, NULL)

static inline int do_comm_call(struct command *comm,
			       char *key safe, struct pane *focus safe, int num, struct mark *m,
			       char *str, int num2, struct mark *m2, char *str2,
			       struct command *comm2, int x, int y, struct pane *home)
{
	struct cmd_info ci = {.key = key, .focus = focus, .home = home ?: focus,
			      .num = num, .mark = m, .str = str,
			      .num2 = num2, .mark2 = m2, .str2 = str2,
			      .comm2 = comm2, .x = x, .y = y,
			      .comm = safe_cast 0};

	if (!comm)
		return -1;
	ci.comm = comm;
	return ci.comm->func(&ci);
}

/* pane_call() is a cross between comm_call and home_call.  The home pane is
 * given first, and home->handle is explicitly called.
 */
#define pane_call(...) VFUNC(pane_call, __VA_ARGS__)
#define pane_call12(home, key, focus, num, mark, str, num2, mark2, str2, comm2, x, y) \
	do_pane_call(home, key, focus, num, mark, str, num2, mark2, str2, comm2, x, y)
#define pane_call10(home, key, focus, num, mark, str, num2, mark2, str2, comm2) \
	do_pane_call(home, key, focus, num, mark, str, num2, mark2, str2, comm2, 0, 0)
#define pane_call9(home, key, focus, num, mark, str, num2, mark2, str2) \
	do_pane_call(home, key, focus, num, mark, str, num2, mark2, str2, NULL, 0, 0)
#define pane_call8(home, key, focus, num, mark, str, num2, mark2) \
	do_pane_call(home, key, focus, num, mark, str, num2, mark2, NULL, NULL, 0, 0)
#define pane_call7(home, key, focus, num, mark, str, num2) \
	do_pane_call(home, key, focus, num, mark, str, num2, NULL, NULL, NULL, 0, 0)
#define pane_call6(home, key, focus, num, mark, str) \
	do_pane_call(home, key, focus, num, mark, str, 0, NULL, NULL, NULL, 0, 0)
#define pane_call5(home, key, focus, num, mark) \
	do_pane_call(home, key, focus, num, mark, NULL, 0, NULL, NULL, NULL, 0, 0)
#define pane_call4(home, key, focus, num) \
	do_pane_call(home, key, focus, num, NULL, NULL, 0, NULL, NULL, NULL, 0, 0)
#define pane_call3(home, key, focus) \
	do_pane_call(home, key, focus, 0, NULL, NULL, 0, NULL, NULL, NULL, 0, 0)

static inline int do_pane_call(struct pane *home,
			       char *key safe, struct pane *focus safe, int num, struct mark *m,
			       char *str, int num2, struct mark *m2, char *str2,
			       struct command *comm2, int x, int y)
{
	struct cmd_info ci = {.key = key, .focus = focus, .home = home ?: focus,
			      .num = num, .mark = m, .str = str,
			      .num2 = num2, .mark2 = m2, .str2 = str2,
			      .comm2 = comm2, .x = x, .y = y,
			      .comm = safe_cast 0};

	if (!home || !home->handle)
		return -1;
	ci.home = home;
	ci.comm = home->handle;
	return ci.comm->func(&ci);
}

/* call_pane() is similar to call(), but returns a pane that was sent via a
 * standard callback.
 */
struct call_return {
	struct command c;
	struct mark *m, *m2;
	char *s;
	struct pane *p;
	int i, i2;
	int x,y;
	struct command *comm;
};

#define call_pane(...) VFUNC(call_pane, __VA_ARGS__)
#define call_pane10(key, focus, num, mark, str, num2, mark2, str2, x, y) \
	do_call_pane(key, focus, num, mark, str, num2, mark2, str2, x, y)
#define call_pane8(key, focus, num, mark, str, num2, mark2, str2) \
	do_call_pane(key, focus, num, mark, str, num2, mark2, str2, 0, 0)
#define call_pane7(key, focus, num, mark, str, num2, mark2) \
	do_call_pane(key, focus, num, mark, str, num2, mark2, NULL, 0, 0)
#define call_pane6(key, focus, num, mark, str, num2) \
	do_call_pane(key, focus, num, mark, str, num2, NULL, NULL, 0, 0)
#define call_pane5(key, focus, num, mark, str) \
	do_call_pane(key, focus, num, mark, str, 0, NULL, NULL, 0, 0)
#define call_pane4(key, focus, num, mark) \
	do_call_pane(key, focus, num, mark, NULL, 0, NULL, NULL, 0, 0)
#define call_pane3(key, focus, num) \
	do_call_pane(key, focus, num, NULL, NULL, 0, NULL, NULL, 0, 0)
#define call_pane2(key, focus) \
	do_call_pane(key, focus, 0, NULL, NULL, 0, NULL, NULL, 0, 0)

/* The first arg of call_ret() is the name of the field to return
 * but using a comm2 callback
 */

#define call_ret(...) VFUNC(call_ret, __VA_ARGS__)
#define call_ret12(ret, key, focus, num, mark, str, num2, mark2, str2, x, y) \
	do_call_ ## ret(key, focus, num, mark, str, num2, mark2, str2, x, y)
#define call_ret9(ret, key, focus, num, mark, str, num2, mark2, str2) \
	do_call_ ## ret(key, focus, num, mark, str, num2, mark2, str2, 0, 0)
#define call_ret8(ret, key, focus, num, mark, str, num2, mark2) \
	do_call_ ## ret(key, focus, num, mark, str, num2, mark2, NULL, 0, 0)
#define call_ret7(ret, key, focus, num, mark, str, num2) \
	do_call_ ## ret(key, focus, num, mark, str, num2, NULL, NULL, 0, 0)
#define call_ret6(ret, key, focus, num, mark, str) \
	do_call_ ## ret(key, focus, num, mark, str, 0, NULL, NULL, 0, 0)
#define call_ret5(ret, key, focus, num, mark) \
	do_call_ ## ret(key, focus, num, mark, NULL, 0, NULL, NULL, 0, 0)
#define call_ret4(ret, key, focus, num) \
	do_call_ ## ret(key, focus, num, NULL, NULL, 0, NULL, NULL, 0, 0)
#define call_ret3(ret, key, focus) \
	do_call_ ## ret(key, focus, 0, NULL, NULL, 0, NULL, NULL, 0, 0)
