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
	short			data_size;	/* only needed by edlib_do_free */

	int			marks;
	int			refs;
	/* timestamp is low bits of time in milliseconds when some
	 * command started.  This makes it easy to check when we
	 * have done too much work
	 */
	unsigned int		timestamp;

	struct pane		*root safe;
	struct command		*handle;
	struct attrset		*attrs;
	struct list_head	notifiers, notifiees;
	union {
		struct doc	doc;
#ifdef PANE_DATA_TYPE
		PANE_DATA_TYPE	data;
#else
		void		*data safe;
#endif
#ifdef DOC_DATA_TYPE
		DOC_DATA_TYPE	doc_data;
#endif
		void		*_data;
	};
};

static inline unsigned int ts_to_ms(struct timespec *ts safe)
{
	return ts->tv_nsec / 1000 / 1000 + ts->tv_sec * 1000;
}

static inline bool pane_too_long(struct pane *p safe, unsigned int msec)
{
	extern bool edlib_timing_allowed;
	struct timespec ts;
	unsigned int duration;
	if (!edlib_timing_allowed)
		return False;
	clock_gettime(CLOCK_MONOTONIC_COARSE, &ts);
	duration = ts_to_ms(&ts) - p->timestamp;
	if (msec < 100)
		msec = 100;
	return (duration > msec);
}

static inline void pane_set_time(struct pane *p safe)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC_COARSE, &ts);
	p->timestamp = ts_to_ms(&ts);
}

static inline struct pane * safe pane_root(struct pane *p safe)
{
	return p->root;
}

static inline struct pane *safe pane_leaf(struct pane *p safe)
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

static inline int do_call_val(enum target_type type, struct pane *home,
			      struct command *comm2a,
			      const char *key safe, struct pane *focus safe,
			      int num,  struct mark *m,  const char *str,
			      int num2, struct mark *m2, const char *str2,
			      int x, int y, struct command *comm2b,
			      struct commcache *ccache)
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
		if (ccache) {
			ci.home = ccache->home;
			ci.comm = ccache->comm;
		}
		ret = key_handle(&ci);
		break;
	case TYPE_pane:
		if (!home->handle || (home->damaged & DAMAGED_DEAD))
			return Efail;
		if (times_up_fast())
			return Efail;
		if (home)
			ci.home = home;
		ci.comm = home->handle;
		ret = ci.comm->func(&ci);
		break;
	case TYPE_comm:
		if (times_up_fast())
			return Efail;
		if (home)
			ci.home = home;
		ci.comm = comm2a;
		ci.comm2 = comm2b;
		ret = ci.comm->func(&ci);
		ccache = NULL;
		break;
	}
	if (ccache) {
		ccache->comm = ci.comm;
		ccache->home = ci.home;
	}
	return ret;
}
