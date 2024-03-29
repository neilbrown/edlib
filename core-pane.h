#include <unistd.h>

struct pane {
	const char		*name; /* For easy discovery in gdb */
	struct pane		*parent safe;
	struct list_head	siblings;
	struct list_head	children;
	struct pane		*focus;
	short			x,y,z;
	short			h,w;
	short			cx, cy;	/* cursor position */
	short			abs_z;

	short			damaged;
	short			alloc_size;
	short			consistency_checks;

	int			marks;
	int			refs;
	/* timestamp is low bits of time in milliseconds when some
	 * command started.  This makes it easy to check when we
	 * have done too much work.
	 * 0 means nothing is running.
	 * 1 means time is exhausted
	 */
	unsigned int		timestamp;

	struct pane		*root safe;
	struct command		*handle;
	struct attrset		*attrs;
	struct list_head	notifiers, notifiees;
	union {
		struct doc	doc;
#ifdef PANE_DATA_TYPE
		PANE_DATA_TYPE	data[1];
#endif
#ifdef PANE_DATA_PTR_TYPE
		PANE_DATA_PTR_TYPE data safe;
#endif
#ifdef DOC_DATA_TYPE
		DOC_DATA_TYPE	doc_data[1];
#endif
#ifdef PANE_DATA_TYPE_2
		PANE_DATA_TYPE_2 data2[1];
#endif
#ifdef PANE_DATA_PTR_TYPE_2
		PANE_DATA_PTR_TYPE_2 data2 safe;
#endif
#ifdef PANE_DATA_TYPE_3
		PANE_DATA_TYPE_3 data3[1];
#endif
#ifdef PANE_DATA_PTR_TYPE_3
		PANE_DATA_PTR_TYPE_3 data3 safe;
#endif
	};
};

bool pane_no_consistency(struct pane *p safe);
bool pane_too_long(struct pane *p safe, unsigned int msec);
void pane_set_time(struct pane *p safe);
static inline void pane_end_time(struct pane *p safe)
{
	p->timestamp = 0;
}

static inline struct pane * safe pane_root(struct pane *p safe)
{
	return p->root;
}

static inline void time_starts(struct pane *p safe)
{
	pane_set_time(pane_root(p));
	alarm(15);
}

static inline void time_ends(struct pane *p safe)
{
	alarm(0);
	pane_end_time(pane_root(p));
}

static inline bool times_up(struct pane *p safe)
{
	return pane_too_long(pane_root(p), 15000);
}

static inline bool times_up_fast(struct pane *p safe)
{
	return pane_root(p)->timestamp == 1;
}

static inline struct pane *safe pane_focus(struct pane *p safe)
{
	struct pane *f;

	while ((f = p->focus) != NULL)
		p = f;
	return p;
}

static inline struct pane *pane_get(struct pane *p safe) safe
{
	p->refs += 1;
	return p;
}
static inline void pane_put(struct pane *p safe)
{
	p->refs -= 1;
	pane_free(p);
}

int do_comm_call(struct command *comm safe, const struct cmd_info *ci safe);
static inline int do_call_val(enum target_type type, struct pane *home,
			      struct command *comm2a,
			      const char *key safe, struct pane *focus safe,
			      int num,  struct mark *m,  const char *str,
			      int num2, struct mark *m2, const char *str2,
			      int x, int y, struct command *comm2b)
{
	struct cmd_info ci = {.key = key, .focus = focus, .home = focus,
			      .num = num, .mark = m, .str = str,
			      .num2 = num2, .mark2 = m2, .str2 = str2,
			      .comm2 = comm2a ?: comm2b, .x = x, .y = y,
			      .comm = safe_cast NULL};
	int ret;

	if ((type == TYPE_pane || type == TYPE_home) && !home)
		return 0;
	if (type == TYPE_comm && !comm2a)
		return 0;
	ASSERT(!comm2a || !comm2b || comm2a == comm2b || type == TYPE_comm);

	switch(type) {
	default:
	case TYPE_home:
		if (home)
			ci.home = home;
		/* fall-through */
	case TYPE_focus:
		ret = key_handle(&ci);
		break;
	case TYPE_pane:
		ci.home = home;
		if (home->handle)
			ci.comm = home->handle;
		ret = do_comm_call(ci.comm, &ci);
		break;
	case TYPE_comm:
		if (home)
			ci.home = home;
		ci.comm = comm2a;
		ci.comm2 = comm2b;
		ret = do_comm_call(ci.comm, &ci);
		break;
	}
	return ret;
}
