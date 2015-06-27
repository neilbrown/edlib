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
 * A text always has a least one allocation, and at least one chunk.
 * Also, the ->txt pointer of a chunk is immutable once set.
 * So when a text becomes empty, a new chunk with ->txt of NULL is created.
 */

#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <memory.h>
#include <locale.h>
#include <wchar.h>

#include "text.h"
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
	int			first:1;
	int			at_start:1;
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
	struct text_chunk *c;
	if (size < 0)
		return 0;
	lseek(fd, 0, SEEK_SET);
	a = text_new_alloc(t, size);
	if (!a)
		return 0;
	// FIXME should I loop??
	a->free = read(fd, a->text, size);

	c = list_first_entry(&t->text, struct text_chunk, lst);
	c->txt = a->text;
	c->start = 0;
	c->end = size;
	return 1;
}

static void text_add_edit(struct text *t, struct text_chunk *target,
			  int *first, int at_start, int len)
{
	struct text_edit *e = malloc(sizeof(*e));

	e->target = target;
	e->first = *first;
	e->at_start = at_start;
	e->len = len;
	*first = 0;
	e->next = t->undo;
	t->undo = e;
}

void text_add_str(struct text *t, struct text_ref *pos, char *str,
		  struct text_ref *start)
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
	int first_edit = 1;

	if (start)
		*start = *pos;

	if (pos->o == pos->c->end &&
	    pos->c->txt + pos->o == a->text + a->free &&
	    (a->size - a->free >= len ||
	     (len = text_round_len(str, a->size - a->free)) > 0)) {
		/* Some of this ('len') can be added to the current chunk */
		memcpy(a->text+a->free, str, len);
		a->free += len;
		pos->c->end += len;
		pos->o += len;
		str += len;
		text_add_edit(t, pos->c, &first_edit, 0, len);
		len = strlen(str);
	}
	if (!len)
		return;
	/* Need a new chunk.  Might need to split the current chunk first.
	 * Old chunk must be first to simplify updating of pointers */
	if (pos->o < pos->c->end) {
		struct text_chunk *c = malloc(sizeof(*c));
		if (pos->o == pos->c->start) {
			/* At the start of a chunk, so create a new one here */
			c->txt = NULL;
			c->start = c->end = 0;
			c->attrs = attr_collect(pos->c->attrs, pos->o, 0);
			attr_trim(&c->attrs, 0);
			list_add_tail(&c->lst, &pos->c->lst);

			if (start && start->c == pos->c && start->o == pos->o) {
				start->c = c;
				start->o = 0;
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
			text_add_edit(t, c, &first_edit, 0, c->end - c->start);
			text_add_edit(t, pos->c, &first_edit, 0, -c->start - c->end);
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
		text_add_edit(t, pos->c, &first_edit, 0, len);
		a->free += len;
		str += len;
	}
}

void text_add_char(struct text *t, struct text_ref *pos, wchar_t ch)
{
	char str[MB_CUR_MAX+1];
	int l = wctomb(str, ch);
	if (l > 0) {
		str[l] = 0;
		text_add_str(t, pos, str, NULL);
	}
}

/* Text insertion and deletion can modify chunks which various
 * marks point to - so those marks will need to be updated.
 * Modification include splitting a chunk, inserting chunks,
 * or deleting chunks.
 * When a chunk is split, the original because the first part.
 * So any mark pointing past the end of that original must be moved
 * the new new chunks.
 * When a chunk is deleted, any mark pointing to a deleted chunk
 * must be redirected to the (new) point of deletion.
 * When a chunk is inserted, marks before the insertion mark must remain
 * before the inserted chunk, marks after must remain after the insertion
 * point.
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

	if (pos->c->start >= pos->c->end) {
		/* This chunk was deleted */
		*pos = *epos;
		return 1;
	}
	if (text_ref_same(pos, epos)) {
		*pos = *spos;
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

	if (pos->c->start >= pos->c->end) {
		/* This chunk was deleted */
		*pos = *epos;
		return 1;
	}
	if (pos->o > pos->c->end) {
		/* This was split */

		c = epos->c;
		list_for_each_entry_from(c, &t->text, lst) {
			if (c->txt == pos->c->txt &&
			    c->start <= pos->o &&
			    c->end >= pos->o) {
				pos->c = c;
				break;
			}
		}
		return 1;
	}
	if (text_ref_same(pos, spos)) {
		*pos = *epos;
		return 1;
	}
	/* This is beyond the change point and no deletion or split
	 * happened here, so all done.
	 */
	return 0;
}

void text_del(struct text *t, struct text_ref *pos, int len)
{
	int first_edit = 1;
	while (len) {
		struct text_chunk *c = pos->c;
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
				pos->c = malloc(sizeof(*pos->c));
				pos->c->txt = NULL;
				pos->c->start = 0;
				pos->c->end = 0;
				pos->c->attrs = 0;
				pos->o = 0;
			}
			__list_del(c->lst.prev, c->lst.next); /* no poison, retain place in list */
			attr_free(&c->attrs);
			text_add_edit(t, c, &first_edit, 0, c->start - c->end);
			len -= c->end - c->start;
			c->end = c->start;
			if (pos->c->txt == NULL) {
				list_add(&c->lst, &t->text);
				text_add_edit(t, c, &first_edit, 0, 0);
				len = 0;
			}
		} else if (pos->o == pos->c->start) {
			/* If the start of the chunk is deleted, just update */
			struct attrset *s;
			c->start += len;
			pos->o = c->start;
			s = attr_copy_tail(c->attrs, c->start);
			attr_free(&c->attrs);
			c->attrs = s;
			text_add_edit(t, c, &first_edit, 1, len);
			len = 0;
		} else if (c->end - pos->o <= len) {
			/* If the end of the chunk is deleted, just update
			 * and move forward */
			int diff = c->end - pos->o;
			len -= diff;
			c->end = pos->o;
			attr_trim(&c->attrs, c->end);
			text_add_edit(t, c, &first_edit, 0, -diff);
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
			text_add_edit(t, c2, &first_edit, 1, c2->end - c2->start);
			len = 0;
		}
	}
}

void text_undo(struct text *t)
{
	struct text_edit *e;

	while ((e = t->undo) != NULL) {

		if (e->target->end == e->target->start) {
			/* need to re-link */
			struct list_head *l = e->target->lst.prev;
			list_add(&e->target->lst, l);
		}
		if (e->at_start)
			e->target->start -= e->len;
		else
			e->target->end -= e->len;
		t->undo = e->next;
		e->next = t->redo;
		t->redo = e;
		if (e->target->start == e->target->end)
			__list_del(e->target->lst.prev, e->target->lst.next);
		if (e->first)
			break;
	}
}

void text_redo(struct text *t)
{
	struct text_edit *e;

	while ((e = t->redo) != NULL) {
		if (e->target->end == e->target->start) {
			/* need to re-link */
			struct list_head *l = e->target->lst.prev;
			list_add(&e->target->lst, l);
		}
		if (e->at_start)
			e->target->start += e->len;
		else
			e->target->end += e->len;
		t->redo = e->next;
		e->next = t->undo;
		t->undo = e;
		if (e->target->start == e->target->end)
			__list_del(e->target->lst.prev, e->target->lst.next);
		if (t->redo && t->redo->first)
			break;
	}
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

int text_ref_same(struct text_ref *r1, struct text_ref *r2)
{
	if (r1->c == r2->c) {
		return r1->o == r2->o;
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
	struct text_chunk *c = malloc(sizeof(*c));
	text_new_alloc(t, 0);
	INIT_LIST_HEAD(&t->text);
	t->undo = t->redo = NULL;
	INIT_HLIST_HEAD(&t->marks);
	INIT_TLIST_HEAD(&t->points, 0);
	t->groups = NULL;
	t->ngroups = 0;

	c->start = c->end = 0;
	c->attrs = NULL;
	list_add(&c->lst, &t->text);
	return t;
}

struct text_ref text_find_ref(struct text *t, int index)
{
	struct text_ref ret;
	ret.c = list_first_entry(&t->text, struct text_chunk, lst);
	ret.o = ret.c->start;
	while (index > 0 &&
	       text_next(t, &ret) != WEOF)
		index -= 1;
	return ret;
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
	{ " ", 5, 5},
	{ "--", 3, 6},
	{ "p me to the", 3, 7},
	{ "---", 1, 9},
	{ "H me to the Worldαβγ", -1, 9},
	{ "old", 1, 10},
	{ "", -2, 9},
	{ "", -2, 7},
	{ "", -2, 6},
	{ "", -2, 5},
	{ "Hello Worldαβγ", -1, 5},
	{ "", -3, 6},
	{ "", -3, 7},
	{ "Help me to the Worldαβγ", -1, 7},
};
int main(int argc, char *argv[])
{
	int rv = 1;
	struct text t = {0};
	struct text_chunk *c = malloc(sizeof(*c));
	struct text_ref r;
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
			text_undo(&t);
		} else if (test[i].pos == -3) {
			text_redo(&t);
		} else if (test[i].str[0] == '-')
			text_del(&t, &r, strlen(test[i].str));
		else
			text_add_str(&t, &r, test[i].str, NULL);

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
