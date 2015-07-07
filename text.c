/*
 * Text.
 *
 * The text of a document is stored in a collection of allocations
 * which are immutable once created.  They are attached to the 'document'
 * and freed only when the document is discard.
 * The current state of the document is represented by a linked list of
 * 'chunks' which each point to part of some allocation.
 *
 * Each chunk can have 'attributes' which add arbitrary annotations to
 * ranges of text.  Unlike the text itself, these are not immutable.  Only
 * the 'current' attributes are stored.  It is assumed that following
 * 'undo', the appropriate attributes can be re-computed.  i.e. they are
 * an optimization.
 *
 * When text is removed from a document, the 'chunk' is modified to
 * reference less text.  If the chunk becomes empty, it is discarded.
 * When text is added to a document a new chunk is created which
 * points to the next free space in the latest allocation, and text is
 * added there.  If the text is being added to the end of a chunk and
 * it already points to the end of the latest allocation, then no new
 * chunk is allocated.
 *
 * Text is assumed to be UTF-8 encoded.  This becomes relevant when adding
 * a string to the document, and it wont all fit in the current allocation.
 * In that case we ensure the first byte that goes in the next allocation
 * matches 0xxxxxxx or 11xxxxxx., not 10xxxxxx.
 *
 * Undo/redo information is stored as a list of edits.  Each edit
 * changes either the start of the end of a 'chunk'. When a chunk becomes
 * empty it is removed from the chunk list.  The 'prev' pointer is preserved
 * so when an undo make it non-empty, it knows where to be added back.
 *
 * A text always has a least one allocation which is created with the text.
 * If the text is empty, there will not be any chunks though, so all text refs
 * will point to NULL.  The NULL chunk is at the end of the text.
 * The ->txt pointer of chunk never changes.  It is set when the chunk is created
 * and then only start and end are changed.
 */

#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <memory.h>
#include <locale.h>
#include <wchar.h>

#include "list.h"
#include "text.h"
#include "mark.h"
#include "attr.h"

struct text_alloc {
	struct text_alloc *next;
	int size;
	int free;
	char text[];
};

#define DEFAULT_SIZE (4096 - sizeof(struct text_alloc))

struct text_chunk {
	char			*txt;
	int			start;
	int			end;
	struct list_head	lst;
	struct attrset		*attrs;
};

/* An 'edit' consists of one or more text_edit structs linked together.
 * The 'first' text_edit in a group has 'first' set.  So when popping
 * off the 'undo' list, we pop until we find the 'first' one.  When
 * popping of the 'redo' list, we pop a first, then any following
 * non-first entries.
 * Each entry identifies a chunk. If 'at_start' is set, the 'len' is
 * added to the 'txt' pointer and subtracted from the len.  Otherwise
 * the len is set.  If this is zero, the chunk is removed from the list.
 */
struct text_edit {
	struct text_chunk	*target;
	struct text_edit	*next;
	unsigned int		first:1;
	unsigned int		at_start:1;
	int			len:30; // bytes add, -ve for removed.
};

/*
 * Given a UTF-8 string and a max len, return a largest len not more
 * than that which does not include a partial code point.
 */
static int text_round_len(char *text, int len)
{
	while (len > 0 &&
	       (text[len] & 0xC0) == 0x80)
		/* Next byte is in the middle of a code-point */
		len--;
	return len;
}

static struct text_alloc *
text_new_alloc(struct text *t, int size)
{
	struct text_alloc *new;
	if (size == 0)
		size = DEFAULT_SIZE;
	new = malloc(size + sizeof(struct text_alloc));
	if (!new)
		return NULL;
	new->next = t->alloc;
	t->alloc = new;
	new->size = size;
	new->free = 0;
	return new;
}

int text_load_file(struct text *t, int fd)
{
	off_t size = lseek(fd, 0, SEEK_END);
	struct text_alloc *a;
	struct text_chunk *c = malloc(sizeof(*c));
	if (size < 0)
		return 0;
	lseek(fd, 0, SEEK_SET);
	a = text_new_alloc(t, size);
	if (!a)
		return 0;
	// FIXME should I loop??
	a->free = read(fd, a->text, size);

	c->txt = a->text;
	c->attrs = NULL;
	c->start = 0;
	c->end = size;
	list_add(&c->lst, &t->text);
	return 1;
}

static void text_add_edit(struct text *t, struct text_chunk *target,
			  int *first, int at_start, int len)
{
	struct text_edit *e;

	if (len == 0)
		return;
	e = malloc(sizeof(*e));
	e->target = target;
	e->first = *first;
	e->at_start = at_start;
	e->len = len;
	*first = 0;
	e->next = t->undo;
	t->undo = e;
}

void text_add_str(struct text *t, struct text_ref *pos, char *str,
		  struct text_ref *start, int *first_edit)
{
	/* Text is added to the end of the referenced chunk, or
	 * in new chunks which are added afterwards.  This allows
	 * the caller to reliably updated any pointers to accommodate
	 * changes.
	 * The attributes of the added text are copied from the preceding
	 * character.  Text added at start of buffer gets no attributes.
	 *
	 * 'pos' is moved to point to the end of the inserted text.
	 * 'start' is set to point to the start which may be the
	 * original 'pos', or may not if a chunk was inserted.
	 */
	/* easy/common case first: pos is at the end of a chunk,
	 * which is the last chunk in the current allocation.
	 */
	struct text_alloc *a = t->alloc;
	int len = strlen(str);

	if (start)
		*start = *pos;

	if (pos->c && pos->o == pos->c->end &&
	    pos->c->txt + pos->o == a->text + a->free &&
	    (a->size - a->free >= len ||
	     (len = text_round_len(str, a->size - a->free)) > 0)) {
		/* Some of this ('len') can be added to the current chunk */
		memcpy(a->text+a->free, str, len);
		a->free += len;
		pos->c->end += len;
		pos->o += len;
		str += len;
		text_add_edit(t, pos->c, first_edit, 0, len);
		len = strlen(str);
	}
	if (!len)
		return;
	/* Need a new chunk.  Might need to split the current chunk first.
	 * Old chunk must be first to simplify updating of pointers */
	if (pos->c == NULL || pos->o < pos->c->end) {
		struct text_chunk *c = malloc(sizeof(*c));
		if (pos->c == NULL || pos->o == pos->c->start) {
			/* At the start of a chunk, so create a new one here */
			c->txt = NULL;
			c->start = c->end = 0;
			if (pos->c) {
				c->attrs = attr_collect(pos->c->attrs, pos->o, 0);
				attr_trim(&c->attrs, 0);
				list_add_tail(&c->lst, &pos->c->lst);
			} else {
				c->attrs = NULL;
				list_add_tail(&c->lst, &t->text);
			}

			if (start && start->c == pos->c && start->o == pos->o) {
				start->c = c;
				start->o = c->start;
			}
			pos->c = c;
			pos->o = c->start;
		} else {
			c->txt = pos->c->txt;
			c->start = pos->o;
			c->end = pos->c->end;
			c->attrs = attr_copy_tail(pos->c->attrs, c->start);
			attr_trim(&pos->c->attrs, c->start);
			pos->c->end = pos->o;
			list_add(&c->lst, &pos->c->lst);
			text_add_edit(t, c, first_edit, 0, c->end - c->start);
			/* this implicitly truncates pos->c, so don't need to record that. */
		}
	}
	while ((len = strlen(str)) > 0) {
		/* Make sure we have an empty chunk */
		if (pos->c->end > pos->c->start) {
			struct text_chunk *c = malloc(sizeof(*c));
			c->start = c->end = 0;
			c->attrs = attr_collect(pos->c->attrs, pos->c->end, 0);
			list_add(&c->lst, &pos->c->lst);
			if (start && start->c == pos->c && start->o == pos->o) {
				start->c = c;
				start->o = 0;
			}
			pos->c = c;
			pos->o = c->start;
		}
		/* Make sure we have some space in 'a' */
		if (a->size - a->free < len &&
		    (len = text_round_len(str, a->size - a->free)) == 0) {
			a = text_new_alloc(t, 0);
			len = strlen(str);
			if (len > a->size)
				len = text_round_len(str, a->size);
		}
		pos->c->txt = a->text + a->free;
		pos->c->end = len;
		pos->o = len;
		memcpy(a->text + a->free, str, len);
		text_add_edit(t, pos->c, first_edit, 0, len);
		a->free += len;
		str += len;
	}
}

void text_add_char(struct text *t, struct text_ref *pos, wchar_t ch, int *first_edit)
{
	char str[MB_CUR_MAX+1];
	int l = wctomb(str, ch);
	if (l > 0) {
		str[l] = 0;
		text_add_str(t, pos, str, NULL, first_edit);
	}
}

/* Text insertion, deletion, and undo can modify chunks which various
 * marks point to - so those marks will need to be updated.
 * Modification include splitting a chunk, inserting chunks,
 * or deleting chunks and recombining chunks (for undo).
 * Also reducing or increasing the range of a chunk.
 * When a chunk is split, the original becomes the first part.
 * So any mark pointing past the end of that original must be moved
 * to the new chunk.
 * When a chunk is deleted, any mark pointing to a deleted chunk
 * must be redirected to the (new) point of deletion.
 * When a chunk is inserted, marks before the insertion mark must remain
 * before the inserted chunk, marks after must remain after the insertion
 * point.
 * When two chunks are recombined it will appear that the second chunk
 * was deleted.
 * When range is reduced, offset must be moved back into range.
 * When range is increased, and this mark is after change, offset in this mark
 * need to line up with changed point.
 *
 * So text_update_prior_after_change() is called on marks before the
 * mark-of-change in reverse order until the function returns zero.
 * If it finds a mark pointing to a deleted chunk, that mark changes to
 * point the same place as the mark-of-change.
 * If it finds a mark at, or immediately after, the mark-of-change,
 * that mark is moved to point to the start of insert.
 *
 * Then text_update_following_after_change() is called on marks after
 * the mark-of-change in order until that function returns zero.
 * If a mark points outside the range of a chunk, the other half of the
 * chunk is found (by walking forward) and the pointer is updated.
 * If a deleted chunk is found, that mark is redirected to the mark-of-change.
 * If a location at the start is found, it is move to the end.
 */

int text_update_prior_after_change(struct text *t, struct text_ref *pos,
				   struct text_ref *spos, struct text_ref *epos)
{

	if (pos->c == NULL) {
		/* Was at the end, now must be at the start of the change */
		*pos = *spos;
		return 0;
	}

	if (pos->c->start >= pos->c->end) {
		/* This chunk was deleted */
		*pos = *epos;
		return 1;
	}
	if (text_ref_same(t, pos, epos)) {
		*pos = *spos;
		return 1;
	}
	if (pos->o < pos->c->start) {
		/* Text deleted from under me */
		pos->o = pos->c->start;
		return 1;
	}
	if (pos->o > pos->c->end) {
		/* Text deleted under me */
		pos->o = pos->c->end;
		return 1;
	}
	/* no insert or delete here, so all done */
	return 0;
}

int text_update_following_after_change(struct text *t, struct text_ref *pos,
				       struct text_ref *spos, struct text_ref *epos)
{
	/* A change has happened between spos and epos. pos should be at or after
	 * epos.
	 */
	struct text_chunk *c;

	if (pos->c == NULL)
		return 0;

	if (pos->c->start >= pos->c->end) {
		/* This chunk was deleted */
		if (epos->c &&
		    pos->c->txt == epos->c->txt &&
		    pos->o >= epos->c->start &&
		    pos->o < epos->c->end)
			/* chunks were rejoined */
			pos->c = epos->c;
		else
			*pos = *epos;
		return 1;
	}
	if (pos->c == epos->c &&
	    pos->o < epos->o) {
		/* Text inserted, need to push forward. */
		pos->o = epos->o;
		return 1;
	}
	if (pos->o < pos->c->start) {
		/* must have been deleted... */
		pos->o = pos->c->start;
		return 1;
	}
	if (pos->o > pos->c->end) {
		/* This was split, or text was deleted off the end */

		c = epos->c;
		if (c)
			list_for_each_entry_from(c, &t->text, lst) {
				if (c->txt == pos->c->txt &&
				    c->start <= pos->o &&
				    c->end >= pos->o) {
					pos->c = c;
					break;
				}
			}
		if (pos->o > pos->c->end)
			/* no split found, so just a delete */
			pos->o = pos->c->end;
		return 1;
	}
	if (text_ref_same(t, pos, spos)) {
		*pos = *epos;
		return 1;
	}
	/* This is beyond the change point and no deletion or split
	 * happened here, so all done.
	 */
	return 0;
}

void text_del(struct text *t, struct text_ref *pos, int len, int *first_edit)
{
	while (len) {
		struct text_chunk *c = pos->c;
		if (c == NULL)
			/* nothing more to delete */
			break;
		if (pos->o == pos->c->start &&
		    len >= pos->c->end - pos->c->start) {
			/* The whole chunk is deleted, simply disconnect it */
			if (c->lst.next != &t->text) {
				pos->c = list_next_entry(c, lst);
				pos->o = pos->c->start;
			} else if (c->lst.prev != &t->text) {
				pos->c = list_prev_entry(c, lst);
				pos->o = pos->c->end;
			} else {
				/* Deleted final chunk */
				pos->c = NULL;
				pos->o = 0;
			}
			__list_del(c->lst.prev, c->lst.next); /* no poison, retain place in list */
			attr_free(&c->attrs);
			text_add_edit(t, c, first_edit, 0, c->start - c->end);
			len -= c->end - c->start;
			c->end = c->start;
		} else if (pos->o == pos->c->start) {
			/* If the start of the chunk is deleted, just update */
			struct attrset *s;
			c->start += len;
			pos->o = c->start;
			s = attr_copy_tail(c->attrs, c->start);
			attr_free(&c->attrs);
			c->attrs = s;
			text_add_edit(t, c, first_edit, 1, len);
			len = 0;
		} else if (c->end - pos->o <= len) {
			/* If the end of the chunk is deleted, just update
			 * and move forward */
			int diff = c->end - pos->o;
			len -= diff;
			c->end = pos->o;
			attr_trim(&c->attrs, c->end);
			text_add_edit(t, c, first_edit, 0, -diff);
			if (c->lst.next != &t->text) {
				pos->c = list_next_entry(c, lst);
				pos->o = pos->c->start;
			} else
				len = 0;
		} else {
			/* must be deleting out of the middle of the chunk.
			 * need to create new chunk for the 'after' bit.
			 */
			struct text_chunk *c2 = malloc(sizeof(*c2));
			c2->txt = c->txt;
			c2->start = pos->o + len;
			c2->end = c->end;
			c->end = pos->o;
			c2->attrs = attr_copy_tail(c->attrs, c2->start);
			attr_trim(&c->attrs, c->end);
			list_add(&c2->lst, &c->lst);
			text_add_edit(t, c2, first_edit, 0, c2->end - c2->start);
			text_add_edit(t, c, first_edit, 0, -len);
			len = 0;
		}
	}
}

/* text_undo and text_redo return:
 * 0 - there are no more changes to do
 * 1 - A change has been done, there are no more parts to it.
 * 2 - A change has been partially undone - call again to undo more.
 *
 * The 'start' and 'end' reported identify the range changed.  For a reversed insertion
 * they will be the same.  If the undo results in the buffer being empty,
 * both start and end will point to a NULL chunk.
 * When undoing a split, both will be at the point of the split.
 */
int text_undo(struct text *t, struct text_ref *start, struct text_ref *end)
{
	struct text_edit *e = t->undo;

	if (!e)
		return 0;

	if (e->target->end == e->target->start) {
		/* need to re-link */
		struct list_head *l = e->target->lst.prev;
		list_add(&e->target->lst, l);
	}
	start->c = end->c = e->target;
	start->o = e->target->end; // incase was deletion at end
	end->o = e->target->start; // incase was deletion at start
	if (e->at_start) {
		e->target->start -= e->len;
		if (e->len > 0)
			/* was deletion, this is insertion */
			start->o = e->target->start;
		else
			/* was insertion - not really possible */
			start->o = end->o = e->target->start;
	} else {
		e->target->end -= e->len;
		if (e->len > 0)
			/* Was insertion, now deleting */
			start->o = end->o = e->target->end;
		else
			/* Was deletion, now inserting */
			end->o = e->target->end;
	}
	t->undo = e->next;
	e->next = t->redo;
	t->redo = e;
	if (e->target->start == e->target->end) {
		/* The undo deletes this chunk, so it must have been inserted,
		 * either as new text or for a chunk-split.
		 * If new text, leave start/end pointing just past the chunk.
		 * if split, leave them at the point of splitting.
		 */
		if (e->target->lst.next == &t->text) {
			end->c = NULL;
			end->o = 0;
		} else {
			end->c = list_next_entry(e->target, lst);
			end->o = end->c->start;
		}
		*start = *end;

		__list_del(e->target->lst.prev, e->target->lst.next);
		/* If this was created for a split, we need to extend the other half */
		if (e->target->lst.prev != &t->text) {
			struct text_chunk *c = list_prev_entry(e->target, lst);
			start->c = end->c = c;
			start->o = end->o = c->end;
			if (c->txt == e->target->txt &&
			    c->end == e->target->start &&
			    !e->at_start)
				c->end += e->len;
		}
	}
	if (e->first)
		return 1;
	else
		return 2;
}

int text_redo(struct text *t, struct text_ref *start, struct text_ref *end)
{
	struct text_edit *e = t->redo;
	int is_split = 0;

	if (!e)
		return 0;

	if (e->target->end == e->target->start) {
		/* need to re-link */
		struct list_head *l = e->target->lst.prev;
		list_add(&e->target->lst, l);
		/* If this is a split, need to truncate prior */
		if (e->target->lst.prev != &t->text) {
			struct text_chunk *c = list_prev_entry(e->target, lst);
			if (c->txt == e->target->txt &&
			    c->end > e->target->start) {
				c->end = e->target->start;
				is_split = 1;
			}
		}
	}
	start->c = end->c = e->target;
	end->o = e->target->start; // incase is insertion at start
	start->o = e->target->end; // incase inserting at end
	if (e->at_start) {
		e->target->start += e->len;
		if (e->len > 0)
			/* deletion at start */
			start->o = end->o = e->target->start;
		else
			/* Insertion at start, not currently possible */
			start->o = e->target->start;
	} else {
		e->target->end += e->len;
		if (e->len > 0)
			/* Insertion at end */
			end->o = e->target->end;
		else if (is_split)
			start->o = end->o = e->target->start;
		else
			/* Deletion at end */
			start->o = end->o = e->target->end;
	}
	t->redo = e->next;
	e->next = t->undo;
	t->undo = e;
	if (e->target->start == e->target->end) {
		/* This chunk is deleted, so leave start/end pointing beyond it */
		if (e->target->lst.next == &t->text) {
			end->c = NULL;
			end->o = 0;
		} else {
			end->c = list_next_entry(e->target, lst);
			end->o = end->c->start;
		}
		*start = *end;

		__list_del(e->target->lst.prev, e->target->lst.next);
	}
	if (t->redo && t->redo->first)
		return 1;
	else
		return 2;
}

static int common_prefix(char *a, char *b, int l)
{
	int i = 0;
	while (i < l &&
	       a[i] == b[i])
		i++;
	return i;
}

/* Compare a string with the text.
 * Update the ref passed all matching chars.
 * Return length that was matched.
 */
int text_str_cmp(struct text *t, struct text_ref *r, char *s)
{
	struct text_chunk *c = r->c;
	int o = r->o;
	int matched = 0;

	if (c == NULL)
		return 0;

	list_for_each_entry_from(c, &t->text, lst) {
		int l = strlen(s);
		if (o == 0)
			o = c->start;
		if (c->end - o < l)
			l = c->end - o;
		l = common_prefix(c->txt+o, s, l);
		matched += l;
		o += l;
		if (s[l] == 0)
			break;
		if (l == c->end - o)
			break;
		s += l;
		o = 0;
	}
	r->c = c;
	r->o = o;
	return matched;
}

wint_t text_next(struct text *t, struct text_ref *r)
{
	wchar_t ret;
	int err;
	mbstate_t ps = {0};

	if (r->c == NULL)
		return WEOF;

	if (r->o >= r->c->end) {
		if (r->c->lst.next == &t->text)
			return WEOF;
		r->c = list_next_entry(r->c, lst);
		r->o = r->c->start;
	}

	err = mbrtowc(&ret, r->c->txt + r->o, r->c->end - r->o, &ps);
	if (err > 0) {
		r->o += err;
		return ret;
	}
	ret = (unsigned char)r->c->txt[r->o++];
	return ret;
}

wint_t text_prev(struct text *t, struct text_ref *r)
{
	wchar_t ret;
	int err;
	mbstate_t ps = {0};

	if (r->c == NULL) {
		if (list_empty(&t->text))
			return WEOF;
		r->c = list_entry(t->text.prev, struct text_chunk, lst);
		r->o = r->c->end;
	}

	if (r->o <= r->c->start) {
		if (r->c->lst.prev == &t->text)
			return WEOF;
		r->c = list_prev_entry(r->c, lst);
		r->o = r->c->end;
	}
	r->o = r->c->start +
		text_round_len(r->c->txt+r->c->start, r->o - r->c->start - 1);
	err = mbrtowc(&ret, r->c->txt + r->o, r->c->end - r->o, &ps);
	if (err > 0)
		return ret;

	ret = (unsigned char)r->c->txt[r->o];
	return ret;
}

int text_ref_same(struct text *t, struct text_ref *r1, struct text_ref *r2)
{
	if (r1->c == r2->c) {
		return r1->o == r2->o;
	}
	if (r1->c == NULL) {
		return (r2->o == r2->c->end &&
			r2->c->lst.next == &t->text);
	}
	if (r2->c == NULL) {
		return (r1->o == r1->c->end &&
			r1->c->lst.next == &t->text);
	}

	if (r1->o == r1->c->end &&
	    r2->o == r2->c->start &&
	    list_next_entry(r1->c, lst) == r2->c)
		return 1;
	if (r1->o == r1->c->start &&
	    r2->o == r2->c->end &&
	    list_prev_entry(r1->c, lst) == r2->c)
		return 1;
	return 0;
}

struct text *text_new(void)
{
	struct text *t = malloc(sizeof(*t));
	text_new_alloc(t, 0);
	INIT_LIST_HEAD(&t->text);
	t->undo = t->redo = NULL;
	INIT_HLIST_HEAD(&t->marks);
	INIT_TLIST_HEAD(&t->points, 0);
	t->groups = NULL;
	t->ngroups = 0;
	return t;
}

int text_add_type(struct text *t, struct command *c)
{
	struct grp *g;
	int ret;
	int i;

	for (ret = 0; ret < t->ngroups; ret++)
		if (t->groups[ret].notify == NULL)
			break;
	if (ret == t->ngroups) {
		/* Resize the group list */
		t->ngroups += 4;
		g = malloc(sizeof(*g) * t->ngroups);
		for (i = 0; i < ret; i++) {
			tlist_add(&g[i].head, GRP_HEAD, &t->groups[i].head);
			tlist_del(&t->groups[i].head);
			g[i].notify = t->groups[i].notify;
		}
		for (; i <t->ngroups; i++) {
			INIT_TLIST_HEAD(&g[i].head, GRP_HEAD);
			g[i].notify = NULL;
		}
		free(t->groups);
		t->groups = g;
		/* now resize all the points */
		points_resize(t);
	}
	t->groups[i].notify = c;
	return ret;
}

void text_del_type(struct text *t, struct command *c)
{
	/* this type should only have points on the list, not typed
	 * marks.  Just delete everything and clear the 'notify' pointer
	 */
	int i;
	for (i = 0; i < t->ngroups; i++)
		if (t->groups[i].notify == c)
			break;
	if (i >= t->ngroups)
		return;
	t->groups[i].notify = NULL;
	while (!tlist_empty(&t->groups[i].head)) {
		struct tlist_head *tl = t->groups[i].head.next;
		if (TLIST_TYPE(tl) != GRP_LIST)
			abort();
		tlist_del_init(tl);
	}
}

char *text_getstr(struct text *t)
{
	struct text_chunk *c;
	char *ret;
	int l = 0;

	list_for_each_entry(c, &t->text, lst)
		l += c->end - c->start;
	ret = malloc(l+1);
	l = 0;
	list_for_each_entry(c, &t->text, lst) {
		memcpy(ret+l, c->txt + c->start, c->end - c->start);
		l += c->end - c->start;
	}
	ret[l] = 0;
	return ret;
}

struct text_ref text_find_ref(struct text *t, int index)
{
	struct text_ref ret;
	if (list_empty(&t->text)) {
		ret.c = NULL;
		ret.o = 0;
		return ret;
	}
	ret.c = list_first_entry(&t->text, struct text_chunk, lst);
	ret.o = ret.c->start;
	while (index > 0 &&
	       text_next(t, &ret) != WEOF)
		index -= 1;
	return ret;
}

int text_advance_towards(struct text *t, struct text_ref *ref, struct text_ref *target)
{
	/* Move 'ref' towards 'target'.
	 * If at end of chunk, step to next chunk, then
	 * advance to 'target' or to end of chunk, whichever comes first.
	 * return:
	 * 0 - reached end of text
	 * 1 - found target
	 * 2 - on a new chunk, keep looking.
	 */
	if (ref->c == target->c) {
		if (ref->o > target->o)
			/* will never find it */
			return 0;
		ref->o = target->o;
		return 1;
	}
	if (ref->c == NULL) {
		if (target->c->lst.next == &t->text &&
		    target->o == target->c->end)
			return 1;
		return 0;
	}
	if (ref->o >= ref->c->end) {
		if (ref->c->lst.next == &t->text) {
			if (target->c == NULL)
				return 1;
			return 0;
		}
		ref->c = list_next_entry(ref->c, lst);
		ref->o = ref->c->start;
	}
	if (ref->c == target->c) {
		if (ref->o > target->o)
			/* will never find it */
			return 0;
		ref->o = target->o;
		return 1;
	}
	ref->o = ref->c->end;
	return 2;
}

int text_retreat_towards(struct text *t, struct text_ref *ref, struct text_ref *target)
{
	/* Move 'ref' towards 'target'.
	 * If at end of chunk, step to next chunk, then
	 * advance to 'target' or to end of chunk, whichever comes first.
	 * return:
	 * 0 - reached start of text
	 * 1 - found target
	 * 2 - on a new chunk, keep looking.
	 */
	if (ref->c == NULL) {
		if (list_empty(&t->text))
			return 0;
		ref->c = list_entry(t->text.prev, struct text_chunk, lst);
		ref->o = ref->c->end;
	}
	if (ref->o <= ref->c->start) {
		if (ref->c->lst.prev == &t->text)
			return 0;
		ref->c = list_prev_entry(ref->c, lst);
		ref->o = ref->c->end;
	}
	if (ref->c == target->c) {
		ref->o = target->o;
		return 1;
	}
	ref->o = ref->c->start;
	return 2;
}

int text_locate(struct text *t, struct text_ref *r, struct text_ref *dest)
{
	/* move back/forward a little from 'r' looking for 'dest'.
	 * return 0 if not found, -1 if dest found before r.
	 * return 1 if dest found after or at r.
	 */
	struct text_chunk *next, *prev;

	if (r->c == NULL) {
		if (dest->c == NULL)
			return 1;
		else
			return -1;
	}
	if (dest->c == NULL)
		return 1;
	if (r->c == dest->c) {
		if (dest->o < r->o)
			return -1;
		else
			return 1;
	}
	next = (r->c->lst.next == &t->text) ? NULL : list_next_entry(r->c, lst);
	prev = (r->c->lst.prev == &t->text) ? NULL : list_prev_entry(r->c, lst);
	if (next == dest->c)
		return 1;
	if (prev == dest->c)
		return -1;

	next = (next == NULL || next->lst.next == &t->text) ? NULL : list_next_entry(next, lst);
	prev = (prev == NULL || prev->lst.prev == &t->text) ? NULL : list_prev_entry(prev, lst);
	if (next == dest->c)
		return 1;
	if (prev == dest->c)
		return -1;
	return 0;
}

static void check_allocated(struct text *t, char *buf, int len)
{
	struct text_alloc *ta = t->alloc;
	for (ta = t->alloc; ta; ta = ta->next) {
		if (buf >= ta->text && buf+len <= ta->text + ta->free)
			return;
	}
	abort();
}

void text_check_consistent(struct text *t)
{
	/* make sure text is consistent, and abort if not.
	 * - each chunk points to allocated space
	 * - no two chunks overlap
	 * - no chunks are empty
	 */
	struct text_chunk *c;

	list_for_each_entry(c, &t->text, lst) {
		check_allocated(t, c->txt, c->end);
		if (c->start >= c->end)
			abort();
		if (c->start < 0)
			abort();
	}
	list_for_each_entry(c, &t->text, lst) {
		struct text_chunk *c2;
		list_for_each_entry(c2, &t->text, lst) {
			if (c2 == c ||
			    c2->txt != c->txt)
				continue;
			if (c->start >= c2->end)
				continue;
			if (c2->start >= c->end)
				continue;
			abort();
		}
	}
}

void text_ref_consistent(struct text *t, struct text_ref *r)
{
	struct text_chunk *c;
	if (r->c == NULL) {
		if (r->o)
			abort();
		return;
	}
	if (r->o > r->c->end)
		abort();
	if (r->o < r->c->start)
		abort();
	list_for_each_entry(c, &t->text, lst)
		if (r->c == c)
			return;
	abort();
}

#ifdef TEST_TEXT

#include <stdlib.h>
#include <stdio.h>

static void text_dump(struct text *t)
{
	struct text_alloc *a;
	struct text_chunk *c;
	struct text_edit *e;
	printf("TEXT:\n");
	printf(" Allocs:\n");
	for (a = t->alloc; a; a = a->next){
		printf("  0x%016p: %d %d\n", a->text, a->size, a->free);
	}
	printf(" Chunks\n");
	list_for_each_entry(c, &t->text, lst) {
		printf("  0x%016p: %.*s (%d-%d)\n",
		       c->txt,
		       c->end - c->start, c->txt + c->start,
		       c->start, c->end);
	}
	printf(" Undo:\n");
	for (e = t->undo; e; e = e->next){
		printf("  %c 0x%016p %c%d\n",
		       e->first?'\\':' ',
		       e->target->txt,
		       e->at_start?'+':' ',
		       e->len);
	}
	printf(" Redo:\n");
	for (e = t->redo; e; e = e->next){
		printf("  %c 0x%016p %c%d\n",
		       e->first?'/':' ',
		       e->target->txt,
		       e->at_start?'+':' ',
		       e->len);
	}
}

struct { char *str; int pos; int undo_check; } test[] = {
	{ "Hello", 0, 1},
	{ "Worldαβγ", 5, 2},
	{ "HelloWorldαβγ", -1, 2},
	{ " ", 5, 4},
	{ "--", 3, 5},
	{ "p me to the", 3, 6},
	{ "---", 1, 8},
	{ "H me to the Worldαβγ", -1, 8},
	{ "old", 1, 9},
	{ "", -2, 8},
	{ "", -2, 6},
	{ "", -2, 5},
	{ "", -2, 4},
	{ "Hello Worldαβγ", -1, 4},
	{ "", -3, 5},
	{ "", -3, 6},
	{ "Help me to the Worldαβγ", -1, 6},
};
int main(int argc, char *argv[])
{
	int rv = 1;
	struct text t = {0};
	struct text_chunk *c = malloc(sizeof(*c));
	struct text_ref r, start, end;
	int i;
	struct text_edit *e;
	wchar_t w;

	setlocale(LC_ALL, "");
	setlocale(LC_CTYPE, "enUS.UTF-8");
	text_new_alloc(&t, 0);
	c->start = c->end = 0;
	c->attrs = NULL;
	INIT_LIST_HEAD(&t.text);
	list_add(&c->lst, &t.text);

	for (i = 0; i < sizeof(test) / sizeof(test[0]); i++) {
		int first = 1;
		int cnt = test[i].pos;
		list_for_each_entry(c, &t.text, lst) {
			if (c->end - c->start >= cnt)
				break;
			cnt -= c->end - c->start;
		}
		if (c == NULL) {
			printf("c == NULL at i=%d\n", i);
			goto out;
		}
		if (c->end - c->start < cnt) {
			printf("c->len = %d < %d == cnt\n",
			       c->end-c->start, cnt);
			goto out;
		}
		r.c = c;
		r.o = c->start + cnt;
		if (test[i].pos == -1) {
			int m;
			r.o = c->start;
			m = text_str_cmp(&t, &r, test[i].str);
			if (test[i].str[m] ||
			    text_next(&t, &r) != WEOF) {
				printf("Text doesn't match at %d: %s\n",
				       i, test[i].str);
				goto out;
			}
		} else if (test[i].pos == -2) {
			while (text_undo(&t, &start, &end) == 2);
		} else if (test[i].pos == -3) {
			while (text_redo(&t, &start, &end) == 2);
		} else if (test[i].str[0] == '-')
			text_del(&t, &r, strlen(test[i].str), &first);
		else
			text_add_str(&t, &r, test[i].str, NULL, &first);

		cnt = 0;
		for (e = t.undo; e; e = e->next)
			cnt++;
		if (test[i].undo_check != cnt) {
			printf("at %i, undo cnt = %d, not %d\n",
			       i, cnt, test[i].undo_check);
			goto out;
		}
	}
	rv = 0;
	r.c = list_first_entry(&t.text, struct text_chunk, lst);
	r.o = 0;
	while ((w = text_next(&t, &r)) != WEOF) {
		printf("%lc", w); printf("<%x>", w);
	}
	printf("\n");
	while ((w = text_prev(&t, &r)) != WEOF) {
		printf("%lc", w); printf("<%x>", w);
	}
	printf("\n");
out:
	text_dump(&t);
	exit(rv);
}
#endif
