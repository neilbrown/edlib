/*
 * Copyright Neil Brown ©2015 <neil@brown.name>
 * May be distributed under terms of GPLv2 - see file:COPYING
 *
 * Generic text document.
 *
 * This allows for a file to be read in, and edited by creating
 * a linked list of chunks of text - the original isn't changed.
 * This means that pointers outside of the edit region are mostly
 * left untouched.
 *
 * Indefinite undo is kept as a record of changes to the list of chunks.
 * New text is added to the end of a list of allocations.
 *
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

#define _GNU_SOURCE /* for asprintf */
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <memory.h>
#include <locale.h>
#include <stdio.h>
#include <fcntl.h>
#include <errno.h>

/* A doc_ref is treated as a pointer to a chunk, and an offset
 * from the start of 'txt'.  So 'o' must be between c->start and
 * c->end inclusive.
 */
#define PRIVATE_DOC_REF

struct doc_ref {
	struct text_chunk *c;
	int o;
};

#include "core.h"
#include "misc.h"

/* text is allocated is large blocks - possibly a whole
 * file or some other large unit being added to a document.
 * For small additions (normal typing) the default allocation
 * size of 4K.
 * When more is allocated than needed, extra can be added on to
 * the end - 'free' is the next index with free space.
 */
struct text_alloc {
	struct text_alloc *prev;
	int size;
	int free;
	char text[];
};

#define DEFAULT_SIZE (4096 - sizeof(struct text_alloc))

/* The text document is a list of text_chunk.
 * The 'txt' pointer is the text[] of a text_alloc.
 * 'start' and 'end' narrow that.
 */
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
 * added to the 'start' pointer (subtracted for undo).  Otherwise
 * the len added to the end.  If the resulting length is zero, the
 * chunk is removed from the list.
 */
struct text_edit {
	struct text_chunk	*target;
	struct text_edit	*next;
	bool			first:1;
	bool			at_start:1;
	signed int		len:30; // bytes add, -ve for removed.
};

/* A text document is a document with allocations, a list
 * of chunks, and some undo info.
 */
struct text {
	struct doc		doc;

	struct text_alloc	*alloc;
	struct list_head	text;
	struct text_edit	*undo, *redo;

	struct stat		stat;
	char			*fname;
	struct text_edit	*saved;
};

static int text_advance_towards(struct text *t, struct doc_ref *ref, struct doc_ref *target);
static int text_retreat_towards(struct text *t, struct doc_ref *ref, struct doc_ref *target);
static int text_ref_same(struct text *t, struct doc_ref *r1, struct doc_ref *r2);
static int text_locate(struct text *t, struct doc_ref *r, struct doc_ref *dest);
static void text_check_consistent(struct text *t);

static struct map *text_map;
/*
 * A text will mostly hold utf-8 so we try to align chunk breaks with
 * Unicode points.  This particularly affects adding new strings to
 * allocations.
 * There is no guarantee that a byte string is UTF-8 though, so
 * We only adjust the length if we can find a start-of-code-point in
 * the last 4 bytes. (longest UTF-8 encoding of 21bit unicode is 4 bytes).
 * A start of codepoint starts with 0b0 or 0b11, not 0b10.
 */
static int text_round_len(char *text, int len)
{
	/* The string at 'text' is *longer* than 'len', or
	 * at least text[len] is defined - it can be nul.  If
	 * [len] isn't the start of a new codepoint, and there
	 * is a start marker in the previous 4 bytes,
	 * move back to there.
	 */
	int i = 0;
	while (i <= len && i <=4)
		if ((text[len-i] & 0xC0) == 0x80)
			/* next byte is inside a UTF-8 code point, so
			 * this isn't a good spot to end. Try further
			 * back */
			i += 1;
		else
			return len-i;
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
	new->prev = t->alloc;
	t->alloc = new;
	new->size = size;
	new->free = 0;
	return new;
}

DEF_CMD(text_load_file)
{
	struct doc *d = ci->home->data;
	int fd = ci->extra;
	char *name = ci->str;
	off_t size = lseek(fd, 0, SEEK_END);
	struct text_alloc *a;
	struct text_chunk *c = NULL;
	int len;
	struct text *t = container_of(d, struct text, doc);

	if (size < 0)
		goto err;
	lseek(fd, 0, SEEK_SET);
	if (size > 0) {
		c = malloc(sizeof(*c));
		a = text_new_alloc(t, size);
		if (!a)
			goto err;
		while (a->free < size &&
		       (len = read(fd, a->text + a->free, size - a->free)) > 0)
			a->free += len;

		c->txt = a->text;
		c->attrs = NULL;
		c->start = 0;
		c->end = a->free;
		list_add(&c->lst, &t->text);
	}
	if (name) {
		char *dname;

		fstat(fd, &t->stat);
		free(t->fname);
		t->fname = strdup(name);
		dname = strrchr(name, '/');
		if (dname)
			dname += 1;
		else
			dname = name;
		call5("doc:set-name", ci->home, 0, NULL, dname, 0);
	}
	t->saved = t->undo;
	call3("doc:status-changed", ci->home, 0, NULL);
	return 1;
err:
	free(c);
	return 0;
}

static int do_text_write_file(struct text *t, char *fname)
{
	/* Don't worry about links for now
	 * Create a temp file with #basename#~, write to that,
	 * fsync and then rename
	 */
	char *tempname = malloc(strlen(fname) + 3 + 10);
	char *base, *tbase;
	int cnt = 0;
	int fd = -1;
	struct text_chunk *c;

	strcpy(tempname, fname);
	base = strrchr(fname, '/');
	if (base)
		base += 1;
	else
		base = fname;
	tbase = tempname + (base - fname);
	while (cnt < 20 && fd == -1) {
		if (cnt)
			sprintf(tbase, "#%s#~%d", base, cnt);
		else
			sprintf(tbase, "#%s#~", base);
		fd = open(tempname, O_WRONLY|O_CREAT|O_EXCL, 0666);
		if (fd < 0 && errno != EEXIST)
			break;
		cnt += 1;
	}
	if (fd < 0)
		return -1;

	list_for_each_entry(c, &t->text, lst) {
		char *s = c->txt + c->start;
		int ln = c->end - c->start;
		if (write(fd, s, ln) != ln)
			goto error;
	}
	if (fsync(fd) != 0)
		goto error;
	if (rename(tempname, fname) < 0)
		goto error;
	close(fd);
	free(tempname);
	return 0;
error:
	close(fd);
	unlink(tempname);
	free(tempname);
	return -1;

}

DEF_CMD(text_save_file)
{
	struct doc *d = ci->home->data;
	struct text *t = container_of(d, struct text, doc);
	int ret;
	char *msg;
	int change_status = 0;

	if (!t->fname) {
		asprintf(&msg, "** No file name known for %s ***", d->name);
		ret = -1;
	} else {
		ret = do_text_write_file(t, t->fname);
		if (ret == 0) {
			asprintf(&msg, "Successfully wrote %s", t->fname);
			t->saved = t->undo;
			change_status = 1;
		} else
			asprintf(&msg, "*** Faild to write %s ***", t->fname);
	}
	call5("Message", ci->focus, 0, NULL, msg, 0);
	free(msg);
	if (change_status)
		call3("doc:status-changed", d->home, 0, NULL);
	if (ret == 0)
		return 1;
	return -1;
}

DEF_CMD(text_same_file)
{
	struct doc *d = ci->home->data;
	struct text *t = container_of(d, struct text, doc);
	struct stat stb;
	int fd = ci->extra;

	if (t->fname == NULL)
		return 0;
	if (fstat(fd, &stb) != 0)
		return 0;
	if (t->stat.st_ino == stb.st_ino &&
	    t->stat.st_dev == stb.st_dev)
		return 1;
	return 0;
}

static void text_add_edit(struct text *t, struct text_chunk *target,
			  bool *first, int at_start, int len)
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

static void text_add_str(struct text *t, struct doc_ref *pos, char *str,
			 struct doc_ref *start, bool *first_edit)
{
	/* Text is added to the end of the referenced chunk, or
	 * in new chunks which are added afterwards.  This allows
	 * the caller to reliably updated any pointers to accommodate
	 * changes.
	 * The added text has no attributes.
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
	int len2;

	if (start)
		*start = *pos;

	len2 = len;
	if (pos->c && pos->o == pos->c->end &&
	    pos->c->txt + pos->o == a->text + a->free &&
	    (a->size - a->free >= len ||
	     (len2 = text_round_len(str, a->size - a->free)) > 0)) {
		/* Some of this ('len') can be added to the current chunk */
		len = len2;
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
			c->attrs = NULL;
			if (pos->c)
				list_add_tail(&c->lst, &pos->c->lst);
			else
				list_add_tail(&c->lst, &t->text);

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
			c->attrs = NULL;
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
 * was deleted.  In this case though, references to the second chunk need
 * to be repositioned in the first.
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

static int text_update_prior_after_change(struct text *t, struct doc_ref *pos,
					  struct doc_ref *spos, struct doc_ref *epos)
{

	if (pos->c == NULL) {
		/* Was at the end, now must be at the start of the change */
		*pos = *spos;
		return 1;
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

static int text_update_following_after_change(struct text *t, struct doc_ref *pos,
					      struct doc_ref *spos, struct doc_ref *epos)
{
	/* A change has happened between spos and epos. pos should be at or after
	 * epos.
	 */
	struct text_chunk *c;

	if (pos->c == NULL)
		return 1;

	if (pos->c->start >= pos->c->end) {
		/* This chunk was deleted */
		if (epos->c &&
		    pos->c->txt == epos->c->txt &&
		    pos->o >= epos->c->start &&
		    pos->o <= epos->c->end)
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

static void text_del(struct text *t, struct doc_ref *pos, int len, bool *first_edit)
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
			if (len && c->lst.next != &t->text) {
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
static int text_undo(struct text *t, struct doc_ref *start, struct doc_ref *end)
{
	struct text_edit *e = t->undo;
	int status_change = 0;

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
	if (t->undo == t->saved || e->next == t->saved)
		status_change = 1;
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
	if (status_change)
		call3("doc:status-changed", t->doc.home, 0, NULL);
	if (e->first)
		return 1;
	else
		return 2;
}

static int text_redo(struct text *t, struct doc_ref *start, struct doc_ref *end)
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

DEF_CMD(text_reundo)
{
	struct doc *d = ci->home->data;
	struct mark *m = ci->mark;
	bool redo = ci->numeric != 0;
	struct doc_ref start, end;
	int did_do = 2;
	bool first = 1;
	struct text *t = container_of(d, struct text, doc);

	while (did_do != 1) {
		struct mark *m2;
		struct mark *early = NULL;
		int where;
		int i;

		if (redo)
			did_do = text_redo(t, &start, &end);
		else
			did_do = text_undo(t, &start, &end);
		if (did_do == 0)
			break;

		if (first) {
			/* Not nearby, look from the start */
			mark_reset(d, m);
			where = 1;
			first = 0;
		} else
			where = text_locate(t, &m->ref, &end);
		if (!where)
			break;

		if (where == 1) {
			do {
				struct doc_ref tmp = m->ref;
				i = text_advance_towards(t, &tmp, &end);
				if (i == 0)
					break;
				while ((m2 = doc_next_mark_all(m)) != NULL &&
				       m2->ref.c == tmp.c &&
				       m2->ref.o <= tmp.o)
					mark_forward_over(m, m2);
				m->ref = tmp;
			} while (i == 2);
		} else {
			do {
				struct doc_ref tmp = m->ref;
				i = text_retreat_towards(t, &tmp, &end);
				if (i == 0)
					break;
				while ((m2 = doc_prev_mark_all(m)) != NULL &&
				       m2->ref.c == tmp.c &&
				       m2->ref.o >= tmp.o)
					mark_backward_over(m, m2);
				m->ref = tmp;
			} while (i == 2);
		}

		if (!text_ref_same(t, &m->ref, &end))
			/* eek! */
			break;
		/* point is now at location of undo */

		m2 = m;
		hlist_for_each_entry_continue_reverse(m2, all)
			if (text_update_prior_after_change(t, &m2->ref,
							   &start, &end) == 0)
				break;
		m2 = m;
		hlist_for_each_entry_continue(m2, all)
			if (text_update_following_after_change(t, &m2->ref,
							       &start, &end) == 0)
				break;

		early = doc_prev_mark_all(m);
		if (early && !text_ref_same(t, &early->ref, &start))
			early = NULL;

		doc_notify_change(&t->doc, ci->mark, early);

		text_check_consistent(t);
	}
	text_check_consistent(t);
	return did_do;
}

#ifdef DEBUG

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
static int text_str_cmp(struct text *t, struct doc_ref *r, char *s)
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
#endif

static wint_t text_next(struct text *t, struct doc_ref *r)
{
	wchar_t ret;
	int err;
	mbstate_t ps = {};

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
		ASSERT(text_round_len(r->c->txt, r->o+err-1) == r->o);
		r->o += err;
		return ret;
	}
	ret = (unsigned char)r->c->txt[r->o++];
	return ret;
}

static wint_t text_prev(struct text *t, struct doc_ref *r)
{
	wchar_t ret;
	int err;
	mbstate_t ps = {};

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

DEF_CMD(text_step)
{
	struct doc *d = ci->home->data;
	struct mark *m = ci->mark;
	struct mark *m2, *target = m;
	bool forward = ci->numeric;
	bool move = ci->extra;
	struct text *t = container_of(d, struct text, doc);
	struct doc_ref r;
	wint_t ret;

	r = m->ref;
	if (forward) {
		ret = text_next(t, &r);
		if (move)
			for (m2 = doc_next_mark_all(m);
			     m2 &&
				     (text_ref_same(t, &m2->ref, &m->ref) ||
				      text_ref_same(t, &m2->ref, &r));
			     m2 = doc_next_mark_all(m2))
				target = m2;
	} else {
		ret = text_prev(t, &r);
		if (move)
			for (m2 = doc_prev_mark_all(m);
			     m2 &&
				     (text_ref_same(t, &m2->ref, &m->ref) ||
				      text_ref_same(t, &m2->ref, &r));
			     m2 = doc_prev_mark_all(m2))
				target = m2;
	}
	if (move) {
		mark_to_mark(m, target);
		m->ref = r;
	}
	/* return value must be +ve, so use high bits to ensure this. */
	return CHAR_RET(ret);
}

static int text_ref_same(struct text *t, struct doc_ref *r1, struct doc_ref *r2)
{
	if (r1->c == r2->c) {
		if (r1->o == r2->o)
			return 1;
		if (r1->c == NULL)
			return 0;
		/* if references are in the middle of a UTF-8 encoded
		 * char, accept as same if it is same char
		 */
		if (r1->o == r1->c->end ||
		    r2->o == r2->c->end)
			return 0;
		return text_round_len(r1->c->txt, r1->o) ==
			text_round_len(r1->c->txt, r2->o);
	}
	if (r1->c == NULL) {
		if (list_empty(&t->text))
			return 1;
		return (r2->o == r2->c->end &&
			r2->c->lst.next == &t->text);
	}
	if (r2->c == NULL) {
		if (list_empty(&t->text))
			return 1;
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

DEF_CMD(text_mark_same)
{
	struct doc *d = ci->home->data;
	struct text *t = container_of(d, struct text, doc);

	return text_ref_same(t, &ci->mark->ref, &ci->mark2->ref) ? 1 : 2;
}

DEF_LOOKUP_CMD_DFLT(text_handle, text_map, doc_default_cmd);

DEF_CMD(text_new)
{
	struct text *t = malloc(sizeof(*t));
	struct pane *p;

	t->alloc = NULL;
	INIT_LIST_HEAD(&t->text);
	t->saved = t->undo = t->redo = NULL;
	doc_init(&t->doc);
	t->fname = NULL;
	text_new_alloc(t, 0);
	p = pane_register(ci->home, 0, &text_handle.c, &t->doc, NULL);
	t->doc.home = p;
	if (p)
		return comm_call(ci->comm2, "callback:doc", p, 0, NULL, NULL, 0);
	return -1;
}

DEF_CMD(text_new2)
{
	if (ci->extra != S_IFREG)
		return 0;
	return text_new_func(ci);
}

static int count_bytes(struct text *t, struct mark *from, struct mark *to)
{
	struct text_chunk *c, *first, *last;
	int l = 0, head, tail;

	first = list_first_entry_or_null(&t->text, struct text_chunk, lst);
	head = 0;
	if (from && from->ref.c) {
		first = from->ref.c;
		head = from->ref.o - first->start;
	}
	last = NULL;
	tail = 0;
	if (to && to->ref.c) {
		last = to->ref.c;
		tail = last->end - to->ref.o;
	}
	c = first;
	list_for_each_entry_from(c, &t->text, lst) {
		l += c->end - c->start;
		if (c == first)
			l -= head;
		if (c == last) {
			l -= tail;
			break;
		}
	}
	return l;
}

DEF_CMD(text_get_str)
{
	struct doc *d = ci->home->data;
	struct mark *from = NULL, *to = NULL;
	struct text *t = container_of(d, struct text, doc);
	struct text_chunk *c, *first, *last;
	char *ret;
	int l = 0, head, tail;

	if (ci->mark && ci->mark2) {
		if (mark_ordered(ci->mark2, ci->mark)) {
			from = ci->mark2;
			to = ci->mark;
		} else {
			from = ci->mark;
			to = ci->mark2;
		}
	}

	first = list_first_entry_or_null(&t->text, struct text_chunk, lst);
	head = 0;
	if (from && from->ref.c) {
		first = from->ref.c;
		head = from->ref.o - first->start;
	}
	last = NULL;
	tail = 0;
	if (to && to->ref.c) {
		last = to->ref.c;
		tail = last->end - to->ref.o;
	}
	c = first;
	list_for_each_entry_from(c, &t->text, lst) {
		l += c->end - c->start;
		if (c == first)
			l -= head;
		if (c == last) {
			l -= tail;
			break;
		}
	}
	ret = malloc(l+1);
	l = 0;
	c = first;
	list_for_each_entry_from(c, &t->text, lst) {
		char *s = c->txt + c->start;
		int ln = c->end - c->start;
		if (c == first) {
			s += head;
			ln -= head;
		}
		if (c == last)
			ln -= tail;
		memcpy(ret+l, s, ln);
		l += ln;
		if (c == last)
			break;
	}
	ret[l] = 0;
	comm_call(ci->comm2, "callback:get-str", ci->focus, 0, NULL, ret, 0);
	free(ret);
	return 1;
}

DEF_CMD(text_set_ref)
{
	struct doc *d = ci->home->data;
	struct mark *m = ci->mark;
	struct text *t = container_of(d, struct text, doc);

	if (list_empty(&t->text) || ci->numeric != 1) {
		m->ref.c = NULL;
		m->ref.o = 0;
	} else {
		m->ref.c = list_first_entry(&t->text, struct text_chunk, lst);
		m->ref.o = m->ref.c->start;
	}
	m->rpos = 0;
	return 1;
}

static int text_advance_towards(struct text *t, struct doc_ref *ref, struct doc_ref *target)
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
			if (target->c == NULL) {
				ref->c = NULL;
				ref->o = 0;
				return 1;
			}
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

static int text_retreat_towards(struct text *t, struct doc_ref *ref, struct doc_ref *target)
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

static int text_locate(struct text *t, struct doc_ref *r, struct doc_ref *dest)
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
	for (ta = t->alloc; ta; ta = ta->prev) {
		if (buf >= ta->text && buf+len <= ta->text + ta->free)
			return;
	}
	abort();
}

static void text_ref_consistent(struct text *t, struct doc_ref *r)
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

static void text_check_consistent(struct text *t)
{
	/* make sure text is consistent, and abort if not.
	 * - each chunk points to allocated space
	 * - no two chunks overlap
	 * - no chunks are empty
	 * - every mark points to a valid chunk with valid offset
	 * - all marks are in text order
	 */
	struct text_chunk *c;
	struct mark *m, *prev;
	struct doc *d = &t->doc;

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

	for (m = doc_first_mark_all(d); m; m = doc_next_mark_all(m))
		text_ref_consistent(t, &m->ref);

	prev = NULL;
	for (m = doc_first_mark_all(d); m; m = doc_next_mark_all(m)) {
		if (prev) {
			struct doc_ref r = prev->ref;
			int i;

			while ((i = text_advance_towards(t, &r, &m->ref)) != 1) {
				if (i == 0)
					abort();
			}
		}
		prev = m;
	}
	doc_check_consistent(d);
}

DEF_CMD(text_replace)
{
	struct doc *d = ci->home->data;
	struct text *t = container_of(d, struct text, doc);
	struct mark *pm = ci->mark2;
	struct mark *end = ci->mark;
	char *str = ci->str;
	bool first = ci->extra;
	struct mark *early = NULL;
	int status_change = 0;

	if (!pm) {
		/* Default to insert at end */
		pm = point_new(d);
		__mark_reset(d, pm, 0, 1);
	}

	/* First delete, then insert */
	if (end && !text_ref_same(t, &pm->ref, &end->ref)) {
		struct mark *myend, *m;
		int l;

		if (t->undo == t->saved)
			status_change = 1;

		if (!mark_ordered(pm, end)) {
			myend = mark_dup(pm, 1);
			mark_to_mark(pm, end);
		} else
			myend = mark_dup(end, 1);
		l = count_bytes(t, pm, myend);
		mark_free(myend);
		text_del(t, &pm->ref, l, &first);

		for (m = doc_prev_mark_all(pm);
		     m && text_update_prior_after_change(t, &m->ref, &pm->ref, &pm->ref);
		     m = doc_prev_mark_all(m))
			;
		for (m = doc_next_mark_all(pm);
		     m && text_update_following_after_change(t, &m->ref, &pm->ref, &pm->ref);
		     m = doc_next_mark_all(m))
			;
		text_check_consistent(t);
	}
	early = doc_prev_mark_all(pm);
	if (early && !text_ref_same(t, &early->ref, &pm->ref))
		early = NULL;

	if (str) {
		struct doc_ref start;
		struct mark *m;

		if (t->undo == t->saved)
			status_change = 1;

		text_add_str(t, &pm->ref, str, &start, &first);
		for (m = doc_prev_mark_all(pm);
		     m && text_update_prior_after_change(t, &m->ref, &start, &pm->ref);
		     m = doc_prev_mark_all(m))
			;
		for (m = doc_next_mark_all(pm);
		     m && text_update_following_after_change(t, &m->ref, &start, &pm->ref);
		     m = doc_next_mark_all(m))
			;
		text_check_consistent(t);

	}
	if (status_change)
		call3("doc:status-changed", d->home, 0, NULL);
	doc_notify_change(&t->doc, pm, early);
	if (!ci->mark2)
		mark_free(pm);
	return first ? 1 : 2;
}

static struct attrset *text_attrset(struct doc *d, struct mark *m,
				    bool forward, int *op)
{
	struct text_chunk *c;
	struct text *t = container_of(d, struct text, doc);
	int o;

	c = m->ref.c;
	o = m->ref.o;
	if (forward) {
		if (!c)
			/* EOF */
			return NULL;
		if (o >= c->end) {
			/* End of chunk, need to look at next */
			if (c->lst.next == &t->text)
				return NULL;
			c = list_next_entry(c, lst);
			o = c->start;
		}
	} else {
		if (!c) {
			if (list_empty(&t->text))
				return NULL;
			c = list_entry(t->text.prev, struct text_chunk, lst);
			o = c->end;
		}
		if (o == 0) {
			if (c->lst.prev == &t->text)
				return NULL;
			c = list_entry(c->lst.prev, struct text_chunk, lst);
			o = c->end;
		}
		o -= 1;
	}
	*op = o;
	return c->attrs;
}

static char *__text_get_attr(struct doc *d, struct mark *m,
			     bool forward, char *attr)
{
	int o;
	struct attrset *a = text_attrset(d, m, forward, &o);
	if (!a)
		return NULL;
	return attr_get_str(a, attr, o);
}

static char *text_next_attr(struct doc *d, struct mark *m,
			    bool forward, char *attr, char **valp)
{
	int o;
	struct attrset *a = text_attrset(d, m, forward, &o);
	if (!a)
		return NULL;
	return attr_get_next_key(a, attr, o, valp);
}

DEF_CMD(text_doc_get_attr)
{
	struct doc *d = ci->home->data;
	struct mark *m = ci->mark;
	bool forward = ci->numeric != 0;
	char *attr = ci->str;
	char *val = __text_get_attr(d, m, forward, attr);

	comm_call(ci->comm2, "callback:get_attr", ci->focus, 0, NULL, val, 0);
	return 1;
}

DEF_CMD(text_get_attr)
{
	struct doc *d = ci->home->data;
	struct text *t = container_of(d, struct text, doc);
	char *attr = ci->str;
	char *val;

	if ((val = attr_find(d->home->attrs, attr)) != NULL)
		;
	else if (strcmp(attr, "render-default") == 0)
		val = "lines";
	else if (strcmp(attr, "doc-type") == 0)
		val = "text";
	else if (strcmp(attr, "filename") == 0)
		val = t->fname;
	else if (strcmp(attr, "doc-modified") == 0)
		val = (t->saved != t->undo) ? "yes" : "no";
	else
		return 0;

	comm_call(ci->comm2, "callback:get_attr", ci->focus, 0, NULL, val, 0);
	return 1;
}

DEF_CMD(text_set_attr)
{
	char *attr = ci->str;
	char *val = ci->str2;
	struct text_chunk *c = ci->mark->ref.c;
	struct doc *d = ci->home->data;
	struct text *t = container_of(d, struct text, doc);
	int o = ci->mark->ref.o;

	if (!c)
		/* EOF */
		return -1;
	if (o >= c->end) {
		/* End of chunk, need to look at next */
		if (c->lst.next == &t->text)
			return -1;
		c = list_next_entry(c, lst);
		o = c->start;
	}
	doc_notify_change(&t->doc, ci->mark, NULL);
	return attr_set_str_key(&c->attrs, attr, val, o);
}

DEF_CMD(text_modified)
{
	struct doc *d = ci->home->data;
	struct text *t = container_of(d, struct text, doc);

	if (ci->numeric == 0) {
		if (t->saved == t->undo)
			t->saved = NULL;
		else
			t->saved = t->undo;
	} else if (ci->numeric > 1)
		t->saved = NULL;
	else
		t->saved = t->undo;
	call3("doc:status-changed", d->home, 0, NULL);
	return 1;
}

DEF_CMD(text_destroy)
{
	struct doc *d = ci->home->data;
	struct text *t = container_of(d, struct text, doc);

	while (!list_empty(&t->text)) {
		struct text_chunk *c = list_entry(t->text.next, struct text_chunk, lst);
		list_del(&c->lst);
		attr_free(&c->attrs);
		free(c);
	}
	while (t->alloc) {
		struct text_alloc *ta = t->alloc;
		t->alloc = ta->prev;
		free(ta);
	}
	while (t->undo) {
		struct text_edit *te = t->undo;
		t->undo = te->next;
		free(te);
	}
	while (t->redo) {
		struct text_edit *te = t->redo;
		t->redo = te->next;
		free(te);
	}
	free(t->fname);
	doc_free(d);
	free(t);
	return 1;
}

#define LARGE_LINE 4096

DEF_CMD(render_line_prev)
{
	/* In the process of rendering a line we need to find the
	 * start of line.
	 * Search backwards until a newline or start-of-file is found,
	 * or until a LARGE_LINE boundary has been passed and a further
	 * LARGE_LINE/2 bytes examined with no newline. In that case,
	 * report the boundary.
	 * If RPT_NUM == 1, step back at least one character so we get
	 * the previous line and not the line we are on.
	 * If we hit start-of-file without finding newline, return -1;
	 */
	struct mark *m = ci->mark;
	struct doc *d = ci->home->data;
	struct mark *boundary = NULL;
	int since_boundary;
	int rpt = RPT_NUM(ci);
	wint_t ch;
	int offset = 0;

	while ((ch = mark_prev(d, m)) != WEOF &&
	       (ch != '\n' || rpt > 0) &&
	       (!boundary || since_boundary < LARGE_LINE/2)) {
		rpt = 0;
		if (boundary)
			since_boundary += 1;
		else if (m->ref.o < offset &&
			 m->ref.o >= LARGE_LINE &&
			 (m->ref.o-1) / LARGE_LINE != (offset-1) / LARGE_LINE) {
			/* here is a boundary */
			boundary = mark_dup(m, 1);
		}
		offset = m->ref.o;
	}
	if (ch != WEOF && ch != '\n') {
		/* need to use the boundary */
		if (!boundary)
			return 1;
		mark_to_mark(m, boundary);
		mark_free(boundary);
		return 1;
	}
	if (boundary)
		mark_free(boundary);
	if (ch == WEOF && rpt)
		return -2;
	if (ch == '\n')
		/* Found a '\n', so step back over it for start-of-line. */
		mark_next(d, m);
	return 1;
}

struct attr_stack {
	struct attr_stack	*next;
	char			*attr;
	int			end;
	int			priority;
};

static int find_finished(struct attr_stack *st, int pos, int *nextp)
{
	int depth = 0;
	int fdepth = -1;
	int next = -1;

	for (; st ; st = st->next, depth++) {
		if (st->end <= pos)
			fdepth = depth;
		else if (next < 0 || next > st->end)
			next = st->end;
	}
	*nextp = next;
	return fdepth;
}

static void as_pop(struct attr_stack **fromp, struct attr_stack **top, int depth,
	    struct buf *b)
{
	struct attr_stack *from = *fromp;
	struct attr_stack *to = *top;

	while (from && depth >= 0) {
		struct attr_stack *t;
		buf_concat(b, "</>");
		t = from;
		from = t->next;
		t->next = to;
		to = t;
		depth -= 1;
	}
	*fromp = from;
	*top = to;
}

static void as_repush(struct attr_stack **fromp, struct attr_stack **top,
		      int pos, struct buf *b)
{
	struct attr_stack *from = *fromp;
	struct attr_stack *to = *top;

	while (from) {
		struct attr_stack *t = from->next;
		if (from->end <= pos) {
			free(from->attr);
			free(from);
		} else {
			buf_append(b, '<');
			buf_concat(b, from->attr);
			buf_append(b, '>');
			from->next = to;
			to = from;
		}
		from = t;
	}
	*fromp = from;
	*top = to;
}

static void as_add(struct attr_stack **fromp, struct attr_stack **top,
		   int end, int prio, char *attr)
{
	struct attr_stack *from = *fromp;
	struct attr_stack *to = *top;
	struct attr_stack *new, **here;

	while (from && from->priority > prio) {
		struct attr_stack *t = from->next;
		from->next = to;
		to = from;
		from = t;
	}
	here = &to;
	while (*here && (*here)->priority <= prio)
		here = &(*here)->next;
	new = calloc(1, sizeof(*new));
	new->next = *here;
	new->attr = strdup(attr);
	new->end = end;
	new->priority = prio;
	*here = new;
	*top = to;
	*fromp = from;
}

struct attr_return {
	struct command c;
	struct attr_stack *ast, *tmpst;
	int min_end;
	int chars;
};

DEF_CMD(text_attr_callback)
{
	struct attr_return *ar = container_of(ci->comm, struct attr_return, c);
	as_add(&ar->ast, &ar->tmpst, ar->chars + ci->numeric, ci->extra, ci->str);
	if (ar->min_end < 0 || ar->chars + ci->numeric < ar->min_end)
		ar->min_end = ar->chars + ci->numeric;
	// FIXME ->str2 should be inserted
	return 1;
}

static void call_map_mark(struct pane *f, struct mark *m, struct attr_return *ar)
{
	char *key = "render:";
	char *val;

	while ((key = attr_get_next_key(m->attrs, key, -1, &val)) != NULL)
		call_comm7("map-attr", f, 0, m, key, 0, val, &ar->c);
}

DEF_CMD(render_line)
{
	/* Render the line from 'mark' to the first '\n' or until
	 * 'extra' chars.
	 * Convert '<' to '<<' and if a char has the 'highlight' attribute,
	 * include that between '<>'.
	 */
	struct buf b;
	struct doc *d = ci->home->data;
	struct text *t = container_of(d, struct text, doc);
	struct mark *m = ci->mark;
	struct mark *pm = ci->mark2; /* The location to render as cursor */
	int o = ci->numeric;
	wint_t ch = WEOF;
	int chars = 0;
	int ret;
	struct attr_return ar;
	int add_newline = 0;

	ar.c = text_attr_callback;
	ar.ast = ar.tmpst = NULL;
	ar.min_end = -1;

	if (!m)
		return -1;

	buf_init(&b);
	while (1) {
		char *key, *val;
		int offset = m->ref.o;
		struct mark *m2;

		if (o >= 0 && b.len >= o)
			break;
		if (pm && text_ref_same(t, &m->ref, &pm->ref))
			break;

		if (ar.ast && ar.min_end <= chars) {
			int depth = find_finished(ar.ast, chars, &ar.min_end);
			as_pop(&ar.ast, &ar.tmpst, depth, &b);
		}

		key = "render:";
		ar.chars = chars;
		while ((key = text_next_attr(d, m, 1, key, &val)) != NULL) {
			call_comm7("map-attr", ci->focus, 0, m, key, 0, val,
				   &ar.c);
		}

		/* find all marks "here" - they might be fore or aft */
		for (m2 = doc_prev_mark_all(m); m2 && text_ref_same(t, &m->ref, &m2->ref);
		     m2 = doc_prev_mark_all(m2))
			call_map_mark(ci->focus, m2, &ar);
		for (m2 = doc_next_mark_all(m); m2 && text_ref_same(t, &m->ref, &m2->ref);
		     m2 = doc_next_mark_all(m2))
			call_map_mark(ci->focus, m2, &ar);

		as_repush(&ar.tmpst, &ar.ast, chars, &b);

		ch = mark_next(d, m);
		if (ch == WEOF)
			break;
		if (ch == '\n') {
			add_newline = 1;
			break;
		}
		if (chars > LARGE_LINE/2 &&
		    m->ref.o > offset &&
		    (m->ref.o-1)/LARGE_LINE != (offset-1)/LARGE_LINE)
			break;
		if (ch == '<') {
			if (o >= 0 && b.len+1 >= o) {
				mark_prev(d, m);
				break;
			}
			buf_append(&b, '<');
		}
		if (ch < ' ' && ch != '\t' && ch != '\n') {
			buf_concat(&b, "<fg:red>^");
			buf_append(&b, '@' + ch);
			buf_concat(&b, "</>");
		} else if (ch == 0x7f) {
			buf_concat(&b, "<fg:red>^?</>");
		} else
			buf_append(&b, ch);
		chars++;
	}
	while (ar.ast)
		as_pop(&ar.ast, &ar.tmpst, 100, &b);
	as_repush(&ar.tmpst, &ar.ast, 10000000, &b);
	if (add_newline)
		buf_append(&b, '\n');

	ret = comm_call(ci->comm2, "callback:render", ci->focus, 0, NULL,
			buf_final(&b), 0);
	free(b.b);
	return ret;
}

void edlib_init(struct pane *ed)
{
	call_comm("global-set-command", ed, 0, NULL, "attach-doc-text", 0, &text_new);
	call_comm("global-set-command", ed, 0, NULL, "open-doc-text", 0, &text_new2);

	text_map = key_alloc();
	key_add(text_map, "render-line-prev", &render_line_prev);
	key_add(text_map, "render-line", &render_line);

	key_add(text_map, "doc:load-file", &text_load_file);
	key_add(text_map, "doc:same-file", &text_same_file);
	key_add(text_map, "doc:get-str", &text_get_str);
	key_add(text_map, "doc:free", &text_destroy);
	key_add(text_map, "doc:set-ref", &text_set_ref);
	key_add(text_map, "doc:save-file", &text_save_file);
	key_add(text_map, "doc:reundo", &text_reundo);
	key_add(text_map, "doc:set-attr", &text_set_attr);
	key_add(text_map, "doc:get-attr", &text_doc_get_attr);
	key_add(text_map, "get-attr", &text_get_attr);
	key_add(text_map, "doc:replace", &text_replace);
	key_add(text_map, "doc:mark-same", &text_mark_same);
	key_add(text_map, "doc:step", &text_step);
	key_add(text_map, "doc:modified", &text_modified);
}
