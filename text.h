
#include "list.h"
struct text_chunk;
struct text_alloc;
struct text_edit;

struct text_ref {
	struct text_chunk *c;
	int o;	/* between c->start and c->end inclusive */
};
struct text {
	struct text_alloc	*alloc;
	struct list_head	text;
	struct text_edit	*undo, *redo;
	struct hlist_head	marks;
	struct tlist_head	points, *groups;
	int			ngroups;
};

struct text *text_new(void);
struct text_ref text_find_ref(struct text *t, int index);
int text_load_file(struct text *t, int fd);
void text_add_str(struct text *t, struct text_ref *pos, char *str,
		  struct text_ref *start);
void text_del(struct text *t, struct text_ref *pos, int len);
wint_t text_next(struct text *t, struct text_ref *r);
wint_t text_prev(struct text *t, struct text_ref *r);
int text_ref_same(struct text_ref *r1, struct text_ref *r2);

int text_update_following_after_add(struct text *t, struct text_ref *ipos,
				    struct text_ref *pos);
int text_update_prior_after_add(struct text *t, struct text_ref *ipos,
				struct text_ref *pos, struct text_ref *start);
int text_update_prior_after_del(struct text *t, struct text_ref *dpos,
				struct text_ref *pos);
int text_update_following_after_del(struct text *t, struct text_ref *dpos,
				    struct text_ref *pos);
