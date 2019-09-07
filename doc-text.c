/*
 * Copyright Neil Brown ©2015-2019 <neil@brown.name>
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
 * a cache.  The owner can get notified of changes which imply that
 * attributes may have been lost.
 *
 * When text is removed from a document, the 'chunk' is modified to
 * reference less text.  If the chunk becomes empty, it is removed
 * from the list, but not freed - as it will be in the undo list.
 * When text is added to a document a new chunk is created which
 * points to the next free space in the latest allocation, and text is
 * added there.  If the text is being added to the end of a chunk and
 * it already points to the end of the latest allocation, then no new
 * chunk is allocated.
 *
 * Text is assumed to be UTF-8 encoded.  This becomes relevant when adding
 * a string to the document and it won't all fit in the current allocation.
 * In that case we ensure the first byte that goes in the next allocation
 * matches 0xxxxxxx or 11xxxxxx., not 10xxxxxx.
 *
 * Undo/redo information is stored as a list of edits.  Each edit
 * changes either the start or the end of a 'chunk'. When a chunk becomes
 * empty it is removed from the chunk list.  The 'prev' pointer is preserved
 * so when an undo makes it non-empty, it knows where to be added back.
 *
 * A text always has a least one allocation which is created with the text.
 * If the text is empty, there will not be any chunks though, so all text refs
 * will point to NULL.  The NULL chunk is at the end of the text.
 * The ->txt pointer of a chunk never changes.  It is set when the chunk is created
 * and then only start and end are changed.
 */

#define _GNU_SOURCE /* for asprintf */
#include <unistd.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>
#include <memory.h>
#include <locale.h>
#include <stdio.h>
#include <fcntl.h>
#include <errno.h>

/* A doc_ref is treated as a pointer to a chunk, and an offset
 * from the start of 'txt'.  So 'o' must be between c->start and
 * c->end inclusive.
 * A 'c' of NULL means end of file.
 * The normalized form requires that 'o' does
 * not point to the end of the chunk.
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
 * size is 4K.
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
 * The 'txt' pointer is within the text[] of a text_alloc.
 * 'start' and 'end' narrow that.
 * Each alloc potentially is divided into multiple
 * separate chunks which are never merged.  The only
 * chunk that can change size is the last one allocated,
 * which may grow into the free space.
 */
struct text_chunk {
	char			*txt safe;
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
 *
 * Each edit can have an altnext.
 * For the undo list, the is an alternate redo to reflect a branching
 * change history.
 * For the redo list, this is a second change that happened from the
 * same starting point.  If there is a third change, we insert a no-op
 * edit so as to get an extra altnext.
 * In the undo list, altnext is an alternate forward path.
 * if alt_is_second, then we are currently on the second path, and after
 * undoing it, will go up the first.
 * if !alt_is_second, we are currently on the first path, and
 * don't want to go back up the second (until we undo all the way to the
 * start and try again).
 */
struct text_edit {
	struct text_chunk	*target safe;
	struct text_edit	*next, *altnext;
	bool			first:1;
	bool			at_start:1;
	bool			alt_is_second:1;
	signed int		len:29; // bytes add, -ve for removed.
};

/* A text document is a document with allocations, a list
 * of chunks, and some undo info.
 */
struct text {
	struct doc		doc;

	struct text_alloc	*alloc safe;
	struct list_head	text;
	struct text_edit	*undo, *redo;
	/* If prev_edit is Redo then next edit is ->redo or ->undo->altnext or ->undo
	 * If prev_edit is Undo, then next edit is ->undo->altnext or ->undo
	 * If prev_edit is AltUndo, then next edit is ->undo
	 */
	enum { Redo, Undo, AltUndo } prev_edit;

	struct stat		stat;
	char			*fname;
	struct text_edit	*saved;
	struct auto_save {
		int		changes;
		int		timer_started;
		time_t		last_change;
	} as;
};

static int text_advance_towards(struct text *t safe, struct doc_ref *ref safe,
				struct doc_ref *target safe);
static int text_retreat_towards(struct text *t safe, struct doc_ref *ref safe,
				struct doc_ref *target safe);
static int text_ref_same(struct text *t safe, struct doc_ref *r1 safe,
			 struct doc_ref *r2 safe);
static int _text_ref_same(struct text *t safe, struct doc_ref *r1 safe,
			  struct doc_ref *r2 safe);
static int text_locate(struct text *t safe, struct doc_ref *r safe,
		       struct doc_ref *dest safe);
static void text_check_consistent(struct text *t safe);
static void text_normalize(struct text *t safe, struct doc_ref *r safe);
static void text_cleanout(struct text *t safe);

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
static int text_round_len(char *text safe, int len)
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

static struct text_alloc *safe
text_new_alloc(struct text *t safe, int size)
{
	struct text_alloc *new;
	if (size == 0)
		size = DEFAULT_SIZE;
	new = malloc(size + sizeof(struct text_alloc));
	new->prev = t->alloc;
	t->alloc = new;
	new->size = size;
	new->free = 0;
	return new;
}

DEF_CMD(text_load_file)
{
	struct doc *d = ci->home->data;
	int fd = ci->num2;
	char *name = ci->str;
	off_t size;
	struct text_alloc *a;
	struct text_chunk *c = NULL;
	int len;
	struct text *t = container_of(d, struct text, doc);

	if (t->saved != t->undo)
		return Einval;
	if (fd < 0 && (ci->num & 2) && t->fname) {
		/* re-open existing file name */
		fd = open(t->fname, O_RDONLY);
		name = t->fname;
	}
	if (fd < 0)
		size = 0;
	else {
		size = lseek(fd, 0, SEEK_END);
		lseek(fd, 0, SEEK_SET);
	}
	if (size < 0)
		goto err;
	if ((ci->num & 1) && t->fname && fd >= 0) {
		struct stat stb;

		fstat(fd, &stb);
		if (stb.st_ino == t->stat.st_ino &&
		    stb.st_dev == t->stat.st_dev &&
		    stb.st_size == t->stat.st_size &&
		    stb.st_mtime == t->stat.st_mtime) {
			if (fd != ci->num2)
				close(fd);
			return Efalse;
		}
	}

	if (size > 0) {
		struct mark *m;
		text_cleanout(t);
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
		hlist_for_each_entry(m, &t->doc.marks, all) {
			m->ref.c = c;
			m->ref.o = 0;
		}
	}
	if (name) {
		char *dname;

		fstat(fd, &t->stat);
		if (name != t->fname) {
			free(t->fname);
			t->fname = strdup(name);
			dname = strrchr(name, '/');
			if (dname)
				dname += 1;
			else
				dname = name;
			call("doc:set-name", ci->home, 0, NULL, dname);
		}
	}
	t->saved = t->undo;
	call("doc:Notify:doc:status-changed", ci->home);
	pane_notify("Notify:doc:Replace", t->doc.home);
	if (fd != ci->num2)
		close(fd);
	return 1;
err:
	free(c);
	if (fd != ci->num2)
		close(fd);
	return Efallthrough;
}

static int do_text_output_file(struct text *t safe, struct doc_ref *start,
			       struct doc_ref *end, int fd)
{
	struct text_chunk *c;
	int offset = 0;

	if (start) {
		c = start->c;
		offset = start->o;
	} else
		c = list_first_entry_or_null(&t->text, struct text_chunk, lst);

	list_for_each_entry_from(c, &t->text, lst) {
		char *s = c->txt + c->start;
		int ln = c->end - c->start;
		if (end && end->c == c)
			ln = end->o;
		if (write(fd, s + offset, ln - offset) != ln - offset)
			return -1;
		offset = 0;
		if (end && end->c == c)
			break;
	}
	if (fsync(fd) != 0)
		return Esys;
	return 0;
}

static int do_text_write_file(struct text *t safe, struct doc_ref *start, struct doc_ref *end,
			      char *fname safe)
{
	/* Don't worry about links for now
	 * Create a temp file with #basename#~, write to that,
	 * copy mode across, fsync and then rename
	 */
	char *tempname = malloc(strlen(fname) + 3 + 10);
	char *base, *tbase;
	int cnt = 0;
	int fd = -1;
	struct stat stb;

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
		return Efail;

	if (do_text_output_file(t, start, end, fd) < 0)
		goto error;
	if (stat(fname, &stb) == 0 &&
	    S_ISREG(stb.st_mode))
		/* Preserve modes, but not setuid */
		fchmod(fd, stb.st_mode & 0777);
	if (rename(tempname, fname) < 0)
		goto error;
	fstat(fd, &t->stat);
	close(fd);
	free(tempname);
	return 0;
error:
	close(fd);
	unlink(tempname);
	free(tempname);
	return Efail;

}

static void do_text_autosave(struct text *t safe)
{
	char *tempname;
	char *base, *tbase;
	int fd = -1;

	if (!t->fname)
		return;
	tempname = malloc(strlen(t->fname) + 3 + 10);
	strcpy(tempname, t->fname);
	base = strrchr(t->fname, '/');
	if (base)
		base += 1;
	else
		base = t->fname;
	tbase = tempname + (base - t->fname);
	sprintf(tbase, "#%s#", base);
	if (t->as.changes == 0) {
		unlink(tempname);
		return;
	}
	fd = open(tempname, O_WRONLY|O_CREAT|O_TRUNC, 0666);
	if (fd < 0)
		return;

	if (do_text_output_file(t, NULL, NULL, fd) < 0) {
		close(fd);
		unlink(tempname);
		free(tempname);
		return;
	}
	t->as.changes = 0;
	close(fd);
	free(tempname);
}

DEF_CMD(text_autosave_tick)
{
	struct pane *home = ci->home;
	struct text *t = home->data;

	t->as.timer_started = 0;
	if (!t->fname)
		return Efalse;
	if (t->as.changes == 0)
		/* This will delete the file */
		do_text_autosave(t);
	if (time(NULL) - t->as.last_change >= 30)
		do_text_autosave(t);
	else {
		t->as.timer_started = 1;
		call_comm("event:timer", t->doc.home, &text_autosave_tick,
		          (t->as.last_change + 30 - time(NULL)) * 1000);
	}
	return Efalse;
}

static void text_check_autosave(struct text *t safe)
{
	if (t->undo == t->saved)
		t->as.changes = 0;
	else
		t->as.changes += 1;
	t->as.last_change = time(NULL);
	if (!t->fname)
		return;
	if (t->as.changes > 300 || t->as.changes == 0)
		do_text_autosave(t);
	else if (!t->as.timer_started) {
		t->as.timer_started = 1;
		call_comm("event:timer", t->doc.home, &text_autosave_tick,
		          30 * 1000);
	}
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
		ret = Efail;
	} else {
		ret = do_text_write_file(t, NULL, NULL, t->fname);
		if (ret == 0) {
			asprintf(&msg, "Successfully wrote %s", t->fname);
			t->saved = t->undo;
			change_status = 1;
		} else
			asprintf(&msg, "*** Failed to write %s ***", t->fname);
	}
	call("Message", ci->focus, 0, NULL, msg);
	free(msg);
	if (change_status)
		call("doc:Notify:doc:status-changed", d->home);
	text_check_autosave(t);
	if (ret == 0)
		return 1;
	return Efail;
}

DEF_CMD(text_write_file)
{
	struct doc *d = ci->home->data;
	struct text *t = container_of(d, struct text, doc);
	int ret;

	if (ci->str) {
		ret = do_text_write_file(t,
					 ci->mark ? &ci->mark->ref: NULL,
					 ci->mark2 ? &ci->mark2->ref: NULL,
					 ci->str);
		return ret == 0 ? 1 : Efail;
	}
	if (ci->num >= 0 && ci->num != NO_NUMERIC) {
		ret = do_text_output_file(t,
					  ci->mark ? &ci->mark->ref: NULL,
					  ci->mark2 ? &ci->mark2->ref: NULL,
					  ci->num);
		return ret = 0 ? 1 : Efail;
	}
	return Enoarg;
}

DEF_CMD(text_same_file)
{
	struct doc *d = ci->home->data;
	struct text *t = container_of(d, struct text, doc);
	struct stat stb;
	int fd = ci->num2;

	if (t->fname == NULL)
		return Efallthrough;
	if (ci->str && strcmp(ci->str, t->fname) == 0)
		return 1;
	if (fd < 0 || fstat(fd, &stb) != 0)
		return Efallthrough;
	if (t->stat.st_ino != stb.st_ino ||
	    t->stat.st_dev != stb.st_dev)
		return Efallthrough;
	/* Must check file hasn't changed beneath us */
	if (stat(t->fname, &t->stat) != 0)
		t->stat.st_ino = 0;
	if (t->stat.st_ino == stb.st_ino &&
	    t->stat.st_dev == stb.st_dev)
		return 0;
	return Efallthrough;
}

static void text_add_edit(struct text *t safe, struct text_chunk *target safe,
			  bool *first safe, int at_start, int len)
{
	struct text_edit *e;

	if (len == 0)
		return;

	if (t->redo) {
		/* Cannot add an edit before some redo edits, as they
		 * will get confused.  We need to record the redo history
		 * here in the undo history, possibly allocating
		 * a nop edit (len == 0)
		 */
		if (t->undo == NULL || t->undo->altnext != NULL) {
			e = malloc(sizeof(*e));
			e->target = target; /* ignored */
			e->first = 0;
			e->at_start = 0;
			e->len = 0; /* This is a no-op */
			e->next = t->undo;
			e->altnext = NULL;
			t->undo = e;
		}
		t->undo->altnext = t->redo;
		t->undo->alt_is_second = 0;
		t->redo = NULL;
	}
		
	e = malloc(sizeof(*e));
	e->target = target;
	e->first = *first;
	e->at_start = at_start;
	e->len = len;
	*first = 0;
	e->next = t->undo;
	e->altnext = NULL;
	e->alt_is_second = 0;
	t->undo = e;
}

static void text_add_str(struct text *t safe, struct doc_ref *pos safe, char *str safe,
			 struct doc_ref *start, bool *first_edit safe)
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
		/* Some of this ('len2') can be added to the current chunk */
		memcpy(a->text+a->free, str, len2);
		a->free += len2;
		pos->c->end += len2;
		pos->o += len2;
		str += len2;
		text_add_edit(t, pos->c, first_edit, 0, len2);
		len -= len2;
	}
	if (!len)
		return;
	/* Need a new chunk.  Might need to split the current chunk first.
	 * Old chunk must be first to simplify updating of pointers */
	if (pos->c == NULL || pos->o < pos->c->end) {
		struct text_chunk *c = malloc(sizeof(*c));
		if (pos->c == NULL || pos->o == pos->c->start) {
			/* At the start of a chunk, so create a new one here */
			c->txt = safe_cast NULL;
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
			/* Not at the start, so we need to split at pos->o */
			c->txt = pos->c->txt;
			c->start = pos->o;
			c->end = pos->c->end;
			c->attrs = attr_copy_tail(pos->c->attrs, c->start);
			pos->c->end = pos->o;
			attr_trim(&pos->c->attrs, pos->c->end);
			list_add(&c->lst, &pos->c->lst);
			text_add_edit(t, c, first_edit, 0, c->end - c->start);
			/* this implicitly truncates pos->c, so don't need to record that. */
		}
	}
	while (len > 0) {
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
		len2 = len;
		if (a->size - a->free < len &&
		    (len2 = text_round_len(str, a->size - a->free)) == 0) {
			a = text_new_alloc(t, 0);
			len2 = len;
			if (len2 > a->size)
				len2 = text_round_len(str, a->size);
		}
		pos->c->txt = a->text + a->free;
		pos->c->end = len2;
		pos->o = len2;
		memcpy(pos->c->txt, str, len2);
		text_add_edit(t, pos->c, first_edit, 0, len2);
		a->free += len2;
		str += len2;
		len -= len2;
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

static int text_update_prior_after_change(struct text *t safe, struct doc_ref *pos safe,
					  struct doc_ref *spos safe, struct doc_ref *epos safe)
{
	int ret = 1;

	if (pos->c == NULL)
		/* Was at the end, now must be at the start of the change */
		*pos = *spos;
	else if (pos->c->start >= pos->c->end)
		/* This chunk was deleted */
		*pos = *spos;
	else if (_text_ref_same(t, pos, epos))
		*pos = *spos;
	else if (pos->o < pos->c->start)
		/* Text deleted from under me */
		pos->o = pos->c->start;
	else if (pos->o > pos->c->end)
		/* Text deleted under me */
		pos->o = pos->c->end;
	else if (pos->o == pos->c->end)
		/* This mark is OK, but previous mark might be
		 * at start of next chunk, so keep looking
		 */
		;
	else
		/* no insert or delete here, so all done */
		ret = 0;
	text_normalize(t, pos);
	return ret;
}

static int text_update_following_after_change(struct text *t safe, struct doc_ref *pos safe,
					      struct doc_ref *spos safe, struct doc_ref *epos safe)
{
	/* A change has happened between spos and epos. pos should be at or after
	 * epos.
	 */
	struct text_chunk *c;
	int ret = 1;

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
	} else if (pos->c == epos->c &&
	    pos->o < epos->o)
		/* Text inserted, need to push forward. */
		pos->o = epos->o;
	else if (pos->o < pos->c->start)
		/* must have been deleted... */
		pos->o = pos->c->start;
	else if (pos->o > pos->c->end) {
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
	} else if (_text_ref_same(t, pos, spos))
		*pos = *epos;
	else if (pos->o == pos->c->start)
		/* This mark is OK, but next mark might be
		 * at end of previous chunk, so keep looking
		 */
		;
	else
		/* This is beyond the change point and no deletion or split
		 * happened here, so all done.
		 */
		ret = 0;
	text_normalize(t, pos);
	return ret;
}

static void text_del(struct text *t safe, struct doc_ref *pos safe, int len,
		     bool *first_edit safe)
{
	while (len) {
		struct text_chunk *c = pos->c;
		if (c == NULL)
			/* nothing more to delete */
			break;
		if (pos->o == c->start &&
		    len >= c->end - c->start) {
			/* The whole chunk is deleted, simply disconnect it */
			if (c != list_last_entry(&t->text, struct text_chunk, lst)) {
				pos->c = list_next_entry(c, lst);
				pos->o = pos->c->start;
			} else if (c != list_first_entry(&t->text, struct text_chunk, lst)) {
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
			/* make sure undo knows this is empty at not attached */
			c->end = c->start;
		} else if (pos->o == c->start) {
			/* If the start of the chunk is deleted, just update.
			 * Note that len must be less that full size, else previous
			 * branch would have been taken.
			 */
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
			if (len && c != list_last_entry(&t->text, struct text_chunk, lst)) {
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
			/* This implicitly trims c, so we only have len left to trim */
			text_add_edit(t, c2, first_edit, 0, c2->end - c2->start);
			text_add_edit(t, c, first_edit, 0, -len);
			len = 0;
		}
	}
}

/* text_undo and text_redo:
 *
 * The 'start' and 'end' reported identify the range changed.  For a reversed insertion
 * they will be the same.  If the undo results in the buffer being empty,
 * both start and end will point to a NULL chunk.
 * When undoing a split, both will be at the point of the split.
 */
static void text_undo(struct text *t safe, struct text_edit *e safe,
                      struct doc_ref *start safe, struct doc_ref *end safe)
{
	if (e->len == 0)
		/* no-op */
		return;
	if (e->target->end == e->target->start) {
		/* need to re-link */
		struct list_head *l = e->target->lst.prev;
		if (e->target->lst.next != l->next) abort();
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
	if (e->target->start == e->target->end) {
		/* The undo deletes this chunk, so it must have been inserted,
		 * either as new text or for a chunk-split.
		 * If new text, leave start/end pointing just past the chunk.
		 * if split, leave them at the point of splitting.
		 */
		if (e->target == list_last_entry(&t->text, struct text_chunk, lst)) {
			end->c = NULL;
			end->o = 0;
		} else {
			end->c = list_next_entry(e->target, lst);
			end->o = end->c->start;
		}
		*start = *end;

		__list_del(e->target->lst.prev, e->target->lst.next);
		/* If this was created for a split, we need to extend the other half */
		if (e->target != list_first_entry(&t->text, struct text_chunk, lst)) {
			struct text_chunk *c = list_prev_entry(e->target, lst);
			start->c = end->c = c;
			start->o = end->o = c->end;
			if (c->txt == e->target->txt &&
			    c->end == e->target->start &&
			    !e->at_start)
				c->end += e->len;
		}
	}
}

static void text_redo(struct text *t safe, struct text_edit *e safe,
                      struct doc_ref *start safe, struct doc_ref *end safe)
{
	int is_split = 0;

	if (e->len == 0)
		/* no-op */
		return;

	if (e->target->end == e->target->start) {
		/* need to re-link */
		struct list_head *l = e->target->lst.prev;
		if (e->target->lst.next != l->next) abort();
		list_add(&e->target->lst, l);
		/* If this is a split, need to truncate prior */
		if (e->target != list_first_entry(&t->text, struct text_chunk, lst)) {
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
		if (is_split)
			start->o = end->o = e->target->start;
		else if (e->len > 0)
			/* Insertion at end */
			end->o = e->target->end;
		else
			/* Deletion at end */
			start->o = end->o = e->target->end;
	}
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
}

DEF_CMD(text_reundo)
{
	struct doc *d = ci->home->data;
	struct mark *m = ci->mark;
	struct doc_ref start, end;
	int last = 0;
	struct text_edit *ed = NULL;
	bool first = 1;
	int status;
	struct text *t = container_of(d, struct text, doc);

	if (!m)
		return Enoarg;

	if (!ci->num)
		/* New undo sequence - do redo first */
		t->prev_edit = Redo;

	status = (t->undo == t->saved);

	do {
		struct mark *m2;
		struct mark *early = NULL;
		int where = 0;
		int i;

		ed = NULL;
		if (t->prev_edit <= Redo && t->redo) {
			ed = t->redo;
			text_redo(t, ed, &start, &end);
			t->redo = ed->next;
			ed->next = t->undo;
			ed->alt_is_second = 0;
			t->prev_edit = Redo;
			t->undo = ed;
			last = t->redo == NULL || t->redo->first;
		} else if (t->prev_edit <= Undo &&
		           t->undo &&
		           t->undo->altnext && !t->undo->alt_is_second) {
			ed = t->undo->altnext;
			text_redo(t, ed, &start, &end);
			t->prev_edit = Redo;
			t->undo->altnext = t->redo;
			t->undo->alt_is_second = 1;
			t->redo = ed->next;
			ed->next = t->undo;
			ed->alt_is_second = 0;
			t->undo = ed;
			last = t->redo == NULL || t->redo->first;
		} else if (t->undo) {
			ed = t->undo;
			text_undo(t, ed, &start, &end);
			t->undo = ed->next;
			if (ed->alt_is_second) {
				t->prev_edit = AltUndo;
				ed->next = ed->altnext;
				ed->altnext = t->redo;
			} else {
				t->prev_edit = Undo;
				ed->next = t->redo;
			}
			t->redo = ed;
			last = ed->first;
		}

		if (!ed)
			break;
		if (ed->len == 0)
			/* That was just a no-op, keep going */
			continue;

		text_normalize(t, &start);
		text_normalize(t, &end);

		if (!first)
			where = text_locate(t, &m->ref, &end);
		if (!where) {
			/* Not nearby, look from the start */
			mark_reset(d, m, 0);
			where = 1;
			first = 0;
		}

		if (where == 1) {
			do {
				struct doc_ref tmp = m->ref;
				i = text_advance_towards(t, &tmp, &end);
				if (i == 0)
					break;
				while ((m2 = doc_next_mark_all(m)) != NULL &&
				       m2->ref.c == tmp.c &&
				       m2->ref.o <= tmp.o)
					mark_to_mark_noref(m, m2);
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
					mark_to_mark_noref(m, m2);
				m->ref = tmp;
			} while (i == 2);
		}

		if (!_text_ref_same(t, &m->ref, &end))
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

		text_normalize(t, &m->ref);
		early = doc_prev_mark_all(m);
		if (early && !text_ref_same(t, &early->ref, &start))
			early = NULL;

		pane_notify("Notify:doc:Replace", t->doc.home, 0, ci->mark, NULL,
			    0, early);

		text_check_consistent(t);

	} while (ed && !last);

	text_check_consistent(t);

	if (status != (t->undo == t->saved))
		call("doc:Notify:doc:status-changed", t->doc.home);
	text_check_autosave(t);

	/* Point probably moved, so */
	pane_damaged(ci->focus, DAMAGED_CURSOR);

	if (!ed)
		t->prev_edit = Redo;
	return ed ? 1 : Efalse;
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

static void text_normalize(struct text *t safe, struct doc_ref *r safe)
{
	while (r->c && r->o >= r->c->end) {
		if (r->c->lst.next == &t->text) {
			r->c = NULL;
			r->o = 0;
		} else {
			r->c = list_next_entry(r->c, lst);
			r->o = r->c->start;
		}
	}
}

static void text_denormalize(struct text *t safe, struct doc_ref *r safe)
{
	if (r->c && r->o > r->c->start)
		/* nothing to do */
		return;
	if (r->c == NULL) {
		if (list_empty(&t->text))
			return;
		r->c = list_entry(t->text.prev, struct text_chunk, lst);
		r->o = r->c->end;
		return;
	}
	if (r->c->lst.prev == &t->text)
		return;
	r->c = list_prev_entry(r->c, lst);
	r->o = r->c->end;
}

static wint_t text_next(struct text *t safe, struct doc_ref *r safe, bool bytes)
{
	wchar_t ret;
	int err;
	mbstate_t ps = {};

	text_normalize(t, r);
	if (r->c == NULL)
		return WEOF;

	err = bytes ? 0 : mbrtowc(&ret, r->c->txt + r->o, r->c->end - r->o, &ps);
	if (err > 0) {
		ASSERT(text_round_len(r->c->txt, r->o+err-1) == r->o);
		r->o += err;
	} else
		ret = (unsigned char)r->c->txt[r->o++];
	text_normalize(t, r);
	return ret;
}

static wint_t text_prev(struct text *t safe, struct doc_ref *r safe, bool bytes)
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
	if (bytes)
		r->o -= 1;
	else {
		r->o = r->c->start +
			text_round_len(r->c->txt+r->c->start, r->o - r->c->start - 1);
		err = mbrtowc(&ret, r->c->txt + r->o, r->c->end - r->o, &ps);
		if (err > 0)
			return ret;
	}

	ret = (unsigned char)r->c->txt[r->o];
	return ret;
}

DEF_CMD(text_step)
{
	struct doc *d = ci->home->data;
	struct mark *m = ci->mark;
	struct mark *m2, *target = m;
	bool forward = ci->num;
	bool move = ci->num2;
	struct text *t = container_of(d, struct text, doc);
	struct doc_ref r;
	wint_t ret;

	if (!m)
		return Enoarg;

	r = m->ref;
	if (forward) {
		ret = text_next(t, &r, 0);
		if (move)
			for (m2 = doc_next_mark_all(m);
			     m2 &&
				     (text_ref_same(t, &m2->ref, &m->ref) ||
				      text_ref_same(t, &m2->ref, &r));
			     m2 = doc_next_mark_all(m2))
				target = m2;
	} else {
		ret = text_prev(t, &r, 0);
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

DEF_CMD(text_step_bytes)
{
	struct doc *d = ci->home->data;
	struct mark *m = ci->mark;
	struct mark *m2, *target = m;
	bool forward = ci->num;
	bool move = ci->num2;
	struct text *t = container_of(d, struct text, doc);
	struct doc_ref r;
	wint_t ret;

	if (!m)
		return Enoarg;

	r = m->ref;
	if (forward) {
		ret = text_next(t, &r, 1);
		if (move)
			for (m2 = doc_next_mark_all(m);
			     m2 &&
				     (text_ref_same(t, &m2->ref, &m->ref) ||
				      text_ref_same(t, &m2->ref, &r));
			     m2 = doc_next_mark_all(m2))
				target = m2;
	} else {
		ret = text_prev(t, &r, 1);
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

static int _text_ref_same(struct text *t safe, struct doc_ref *r1 safe, struct doc_ref *r2 safe)
{
	if (r1->c == r2->c) {
#if 1
		return r1->o == r2->o;
#else
		if (r1->o == r2->o)
			return 1;
		if (r1->c == NULL)
			return 0;
		/* FIXME this must fail as r1->c == rc->c, and r1->c != NULL) */
		if (r2->c == NULL)
			return 0;
		/* if references are in the middle of a UTF-8 encoded
		 * char, accept as same if it is same char
		 */
		if (r1->o == r1->c->end ||
		    r2->o == r2->c->end)
			return 0;
		return text_round_len(r1->c->txt, r1->o) ==
			text_round_len(r1->c->txt, r2->o);
#endif
	}
	if (r1->c == NULL /*FIXME redundant*/ && r2->c != NULL) {
		if (list_empty(&t->text))
			return 1;
		return (r2->o == r2->c->end &&
			r2->c->lst.next == &t->text);
	}
	if (r2->c == NULL /* FIXME redundant*/ && r1->c != NULL) {
		if (list_empty(&t->text))
			return 1;
		return (r1->o == r1->c->end &&
			r1->c->lst.next == &t->text);
	}
	/* FIXME impossible */
	if (r1->c == NULL || r2->c == NULL) return 0;

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

static int text_ref_same(struct text *t safe, struct doc_ref *r1 safe, struct doc_ref *r2 safe)
{
	int ret = _text_ref_same(t, r1, r2);
	ASSERT(ret == (r1->c == r2->c && r1->o == r2->o));
	return ret;
}

DEF_LOOKUP_CMD(text_handle, text_map);

DEF_CMD(text_new)
{
	struct text *t = malloc(sizeof(*t));
	struct pane *p;

	t->alloc = safe_cast NULL;
	INIT_LIST_HEAD(&t->text);
	t->saved = t->undo = t->redo = NULL;
	t->prev_edit = Redo;
	t->fname = NULL;
	t->as.changes = 0;
	t->as.timer_started = 0;
	t->as.last_change = 0;
	text_new_alloc(t, 0);
	p = doc_register(ci->home, 0, &text_handle.c, &t->doc, NULL);
	t->doc.home = p;
	if (p)
		return comm_call(ci->comm2, "callback:doc", p);
	return Esys;
}

DEF_CMD(text_new2)
{
	if (ci->num2 != S_IFREG)
		return Efallthrough;
	return text_new_func(ci);
}

static int count_bytes(struct text *t safe, struct mark *from, struct mark *to)
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
		if (ci->mark2->seq < ci->mark->seq) {
			from = ci->mark2;
			to = ci->mark;
		} else {
			from = ci->mark;
			to = ci->mark2;
		}
	}

	first = list_first_entry_or_null(&t->text, struct text_chunk, lst);
	head = 0;
	if (from) {
		first = from->ref.c;
		if (first)
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
	comm_call(ci->comm2, "callback:get-str", ci->focus, 0, NULL, ret);
	free(ret);
	return 1;
}

DEF_CMD(text_set_ref)
{
	struct doc *d = ci->home->data;
	struct mark *m = ci->mark;
	struct text *t = container_of(d, struct text, doc);

	if (!m)
		return Enoarg;
	if (list_empty(&t->text) || ci->num != 1) {
		m->ref.c = NULL;
		m->ref.o = 0;
	} else {
		m->ref.c = list_first_entry(&t->text, struct text_chunk, lst);
		m->ref.o = m->ref.c->start;
	}
	mark_to_end(d, m, ci->num != 1);
	return 1;
}

static int text_advance_towards(struct text *t safe,
				struct doc_ref *ref safe, struct doc_ref *target safe)
{
	/* Move 'ref' towards 'target'.
	 * If at end of chunk, step to next chunk, then
	 * advance to 'target' or to end of chunk, whichever comes first.
	 * return:
	 * 0 - reached end of text
	 * 1 - found target
	 * 2 - on a new chunk, keep looking.
	 */
	if (ref->c && ref->o >= ref->c->end)
		text_normalize(t, ref);
	if (ref->c == target->c) {
		if (ref->o > target->o)
			/* will never find it */
			return 0;
		ref->o = target->o;
		return 1;
	}
	if (ref->c == NULL)
		/* Reached EOF, haven't found */
		return 0;
	ref->o = ref->c->end;
	return 2;
}

static int text_retreat_towards(struct text *t safe, struct doc_ref *ref safe,
				struct doc_ref *target safe)
{
	/* Move 'ref' towards 'target'.
	 * If at start of chunk, step to previous chunk, then
	 * retreat to 'target' or to start of chunk, whichever comes first.
	 * return:
	 * 0 - reached start of text
	 * 1 - found target
	 * 2 - on a new chunk, keep looking.
	 */

	if (ref->c && ref->c != target->c && ref->o <= ref->c->start)
		if (text_prev(t, ref, 1) == WEOF)
			return 0;

	if (ref->c == target->c) {
		if (ref->c == NULL)
			return 1;
		if (target->o > ref->o)
			return 0;
		ref->o = target->o;
		return 1;
	}
	if (ref->c)
		ref->o = ref->c->start;
	return 2;
}

static int text_locate(struct text *t safe, struct doc_ref *r safe, struct doc_ref *dest safe)
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

static void check_allocated(struct text *t safe, char *buf safe, int len)
{
	struct text_alloc *ta = t->alloc;
	for (ta = t->alloc; ta; ta = ta->prev) {
		if (buf >= ta->text && buf+len <= ta->text + ta->free)
			return;
	}
	abort();
}

static void text_ref_consistent(struct text *t safe, struct doc_ref *r safe)
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

static void text_check_consistent(struct text *t safe)
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
			struct doc_ref r = prev->ref;/* SMATCH Bug things prev has no state*/
			int i;
			text_normalize(t, &m->ref);
			while ((i = text_advance_towards(t, &r, &m->ref)) != 1) {
				if (i == 0)
					abort();
			}
		}
		prev = m;
	}
	doc_check_consistent(d);
}

static void text_add_attrs(struct attrset **attrs safe, char *new safe, int o)
{
	char sep = *new++;
	char *cpy = strdup(new);
	char *a = cpy;

	while (a) {
		char *k, *v;
		k = a;
		a = strchr(a, sep);
		if (a)
			*a++ = '\0';
		v = strchr(k, '=');
		if (v)
			*v++ = '\0';
		else
			continue;
		attr_set_str_key(attrs, k, v, o);
	}

	free(cpy);
}

DEF_CMD(text_replace)
{
	struct doc *d = ci->home->data;
	struct text *t = container_of(d, struct text, doc);
	struct mark *pm = ci->mark2;
	struct mark *end = ci->mark;
	char *str = ci->str;
	char *newattrs = ci->str2;
	bool first = ci->num2;
	struct mark *early = NULL;
	int status_change = 0;

	if (!pm) {
		/* Default to insert at end */
		pm = point_new(d);
		mark_reset(d, pm, 1);
	} else
		/* probably move point */
		pane_damaged(ci->focus, DAMAGED_CURSOR);

	/* First delete, then insert */
	if (end && !text_ref_same(t, &pm->ref, &end->ref)) {
		struct mark *myend, *m;
		int l;

		if (t->undo == t->saved)
			status_change = 1;

		if (pm->seq >= end->seq) {
			myend = mark_dup(pm);
			mark_to_mark(pm, end);
		} else
			myend = mark_dup(end);
		l = count_bytes(t, pm, myend);
		mark_free(myend);
		text_del(t, &pm->ref, l, &first);
		text_normalize(t, &pm->ref);

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

		text_denormalize(t, &pm->ref);
		text_add_str(t, &pm->ref, str, &start, &first);
		text_normalize(t, &pm->ref);
		for (m = doc_prev_mark_all(pm);
		     m && text_update_prior_after_change(t, &m->ref, &start, &pm->ref);
		     m = doc_prev_mark_all(m))
			;
		for (m = doc_next_mark_all(pm);
		     m && text_update_following_after_change(t, &m->ref, &start, &pm->ref);
		     m = doc_next_mark_all(m))
			;
		if (newattrs && start.c)
			text_add_attrs(&start.c->attrs, newattrs, start.o);
		text_check_consistent(t);

	}
	text_check_autosave(t);
	if (status_change)
		call("doc:Notify:doc:status-changed", d->home);
	pane_notify("Notify:doc:Replace", t->doc.home, 0, pm, NULL,
		    0, early);
	if (!ci->mark2)
		mark_free(pm);
	return first ? 1 : 2;
}

static struct attrset *text_attrset(struct doc *d safe, struct mark *m safe,
				    int *op safe)
{
	struct text_chunk *c;
	struct text *t = container_of(d, struct text, doc);
	int o;

	c = m->ref.c;
	o = m->ref.o;

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

	*op = o;
	return c->attrs;
}

DEF_CMD(text_doc_get_attr)
{
	struct doc *d = ci->home->data;
	struct mark *m = ci->mark;
	char *attr = ci->str;
	char *val;
	struct attrset *a;
	int o = 0;

	if (!m || !attr)
		return Enoarg;
	a = text_attrset(d, m, &o);
	val = attr_get_str(a, attr, o);
	comm_call(ci->comm2, "callback:get_attr", ci->focus, 0, NULL, val);
	if (ci->num2 == 1) {
		char *key = attr;
		int len = strlen(attr);
		while ((key = attr_get_next_key(a, key, o, &val)) != NULL &&
		       strncmp(key, attr, len) == 0)
			comm_call(ci->comm2, "callback:get_attr", ci->focus, 0, NULL, val, 0,
				   NULL, key);
	}
	return 1;
}

DEF_CMD(text_get_attr)
{
	struct doc *d = ci->home->data;
	struct text *t = container_of(d, struct text, doc);
	char *attr = ci->str;
	char *val;

	if (!attr)
		return Enoarg;

	if ((val = attr_find(d->home->attrs, attr)) != NULL)
		;
	else if (strcmp(attr, "render-default") == 0)
		val = "text";
	else if (strcmp(attr, "doc-type") == 0)
		val = "text";
	else if (strcmp(attr, "doc:charset") == 0)
		val = "utf-8";
	else if (strcmp(attr, "filename") == 0)
		val = t->fname;
	else if (strcmp(attr, "doc-modified") == 0)
		val = (t->saved != t->undo) ? "yes" : "no";
	else
		return Efallthrough;

	comm_call(ci->comm2, "callback:get_attr", ci->focus, 0, NULL, val);
	return 1;
}

DEF_CMD(text_set_attr)
{
	char *attr = ci->str;
	char *val = ci->str2;
	struct text_chunk *c;
	struct doc *d = ci->home->data;
	struct text *t = container_of(d, struct text, doc);
	int o;

	if (!attr)
		return Enoarg;
	if (!ci->mark)
		return Efallthrough;

	o = ci->mark->ref.o;
	c = ci->mark->ref.c;
	if (!c)
		/* EOF */
		return Efallthrough;
	if (o >= c->end) {
		/* End of chunk, need to look at next */
		if (c->lst.next == &t->text)
			return Efallthrough;
		c = list_next_entry(c, lst);
		o = c->start;
	}
	pane_notify("Notify:doc:Replace", ci->home, 0, ci->mark);
	attr_set_str_key(&c->attrs, attr, val, o);
	return Efallthrough;
}

DEF_CMD(text_modified)
{
	struct doc *d = ci->home->data;
	struct text *t = container_of(d, struct text, doc);

	if (ci->num == 0) {
		if (t->saved == t->undo)
			t->saved = NULL;
		else
			t->saved = t->undo;
	} else if (ci->num > 1)
		t->saved = NULL;
	else
		t->saved = t->undo;
	text_check_autosave(t);
	call("doc:Notify:doc:status-changed", d->home);
	return 1;
}

static void text_cleanout(struct text *t safe)
{
	struct text_alloc *ta;
	struct mark *m;

	hlist_for_each_entry(m, &t->doc.marks, all) {
		m->ref.c = NULL;
		m->ref.o = 0;
	}

	while (!list_empty(&t->text)) {
		struct text_chunk *c = list_entry(t->text.next, struct text_chunk, lst);
		list_del(&c->lst);
		attr_free(&c->attrs);
		free(c);
	}
	ta = t->alloc;
	while (ta) {
		struct text_alloc *tmp = ta;
		ta = tmp->prev;
		free(tmp);
	}
	t->alloc = safe_cast NULL;
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
}

DEF_CMD(text_destroy)
{
	struct doc *d = ci->home->data;
	struct text *t = container_of(d, struct text, doc);

	text_cleanout(t);
	free(t->fname);
	doc_free(d);
	free(t);
	return 1;
}

void edlib_init(struct pane *ed safe)
{
	call_comm("global-set-command", ed, &text_new, 0, NULL, "attach-doc-text");
	call_comm("global-set-command", ed, &text_new2, 0, NULL, "open-doc-text");

	text_map = key_alloc();

	key_add_chain(text_map, doc_default_cmd);
	key_add(text_map, "doc:load-file", &text_load_file);
	key_add(text_map, "doc:same-file", &text_same_file);
	key_add(text_map, "doc:get-str", &text_get_str);
	key_add(text_map, "doc:set-ref", &text_set_ref);
	key_add(text_map, "doc:save-file", &text_save_file);
	key_add(text_map, "doc:write-file", &text_write_file);
	key_add(text_map, "doc:reundo", &text_reundo);
	key_add(text_map, "doc:set-attr", &text_set_attr);
	key_add(text_map, "doc:get-attr", &text_doc_get_attr);
	key_add(text_map, "doc:replace", &text_replace);
	key_add(text_map, "doc:step", &text_step);
	key_add(text_map, "doc:step-bytes", &text_step_bytes);
	key_add(text_map, "doc:modified", &text_modified);

	key_add(text_map, "Close", &text_destroy);
	key_add(text_map, "get-attr", &text_get_attr);
}
