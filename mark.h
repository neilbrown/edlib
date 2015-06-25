
struct point;
struct mark;

enum {
	MARK_POINT = -1,
	MARK_UNGROUPED = -2
};

enum {
	GRP_HEAD = 0, // tlist_head list head
	GRP_MARK = 1, // tlist_head in mark.group
	GRP_LIST = 2, // tlist_head in point.lists
};

struct point *point_new(struct text *t, struct point **owner);
struct text_ref point_ref(struct point *p);
wint_t mark_next(struct text *t, struct mark *m);
wint_t mark_prev(struct text *t, struct mark *m);
wchar_t mark_following(struct text *t, struct mark *m);
wchar_t mark_prior(struct text *t, struct mark *m);
struct mark *mark_of_point(struct point *p);
int mark_ordered(struct mark *m1, struct mark *m2);
struct mark *mark_dup(struct mark *m, int notype);
void mark_delete(struct mark *m);
struct mark *mark_at_point(struct point *p, int type);
int mark_same(struct mark *m1, struct mark *m2);
void point_insert_text(struct text *t, struct point *p, char *s);
void point_delete_text(struct text *t, struct point *p, int len);
void point_to_mark(struct text *t, struct point *p, struct mark *m);
struct point *point_dup(struct point *p, struct point **owner);
