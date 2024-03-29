/*
 * Copyright Neil Brown ©2015-2023 <neil@brown.name>
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
 * The ->txt pointer of a chunk never changes.  It is set when the chunk
 * is created and then only start and end are changed.
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
#include <dirent.h>

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
	unsigned int o;
};
struct text;
#define DOC_DATA_TYPE struct text
#define DOC_NEXT(d,m,r,b) text_next(d,r,b)
#define DOC_PREV(d,m,r,b) text_prev(d,r,b)
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

#define DEFAULT_SIZE ((int)(4096 - sizeof(struct text_alloc)))
#define MAX_SIZE ((int)((1<<20) - sizeof(struct text_alloc)))

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
	unsigned int		start;
	unsigned int		end;
	struct list_head	lst;
	struct attrset		*attrs;
};

/* An 'edit' consists of one or more text_edit structs linked together.
 * The 'first' text_edit in a group has 'first' set.  So when popping
 * off the 'undo' list, we pop until we find the 'first' one.  When
 * popping off the 'redo' list, we pop a first, then any following
 * non-first entries.
 * Each entry identifies a chunk. If 'at_start' is set, the 'len' is
 * added to the 'start' pointer (subtracted for undo).  Otherwise
 * the len added to the end.  If the resulting length is zero, the
 * chunk is removed from the list.
 *
 * Each edit can have an altnext.
 * For the undo list, this is an alternate redo to reflect a branching
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
	/* If prev_edit is Redo then next edit is ->redo or ->undo->altnext
	 * or ->undo
	 * If prev_edit is Undo, then next edit is ->undo->altnext or ->undo
	 * If prev_edit is AltUndo, then next edit is ->undo
	 */
	enum { Redo, Undo, AltUndo } prev_edit;

	bool			revising_marks;
	char			file_changed; /* '2' means it has changed, but
					       * we are editing anyway
					       */
	char			newfile; /* file doesn't exist yet */
	bool			autosave_exists;
	struct stat		stat;
	const char		*fname;
	const char		*autosave_name;
	struct text_edit	*saved;
	struct auto_save {
		int		changes;
		int		timer_started;
		time_t		last_change;
	} as;
};

#include "core-pane.h"

static int text_advance_towards(struct text *t safe, struct doc_ref *ref safe,
				struct doc_ref *target safe);
static int text_retreat_towards(struct text *t safe, struct doc_ref *ref safe,
				struct doc_ref *target safe);
static bool text_ref_same(struct text *t safe, struct doc_ref *r1 safe,
			  struct doc_ref *r2 safe);
static bool _text_ref_same(struct text *t safe, struct doc_ref *r1 safe,
			   struct doc_ref *r2 safe);
static int text_locate(struct text *t safe, struct doc_ref *r safe,
		       struct doc_ref *dest safe);
static void text_check_consistent(struct text *t safe);
static void text_normalize(struct text *t safe, struct doc_ref *r safe);
static void text_cleanout(struct text *t safe);
static void text_add_str(struct text *t safe, struct mark *pm safe,
			 const char *str safe, off_t size,
			 bool *first_edit safe);
static void text_check_autosave(struct pane *p safe);
static bool check_readonly(const struct cmd_info *ci safe);

static MEMPOOL(text);
static MEMPOOL(undo);
static struct map *text_map;

static struct text_alloc *safe
text_new_alloc(struct text *t safe, int size)
{
	struct text_alloc *new;
	if (size == 0)
		size = DEFAULT_SIZE;
	size += sizeof(struct text_alloc);
	size = ((size-1) | 255) + 1;
	new = alloc_buf(size, text);
	new->prev = t->alloc;
	t->alloc = new;
	new->size = size - sizeof(struct text_alloc);
	new->free = 0;
	return new;
}

static bool check_file_changed(struct pane *p safe)
{
	struct text *t = p->doc_data;
	struct stat st;

	if (t->file_changed)
		/* '1' means it has changed, '2' means "but we don't care" */
		return t->file_changed == 1;
	if (!t->fname)
		return False;
	if (stat(t->fname, &st) != 0) {
		memset(&st, 0, sizeof(st));
		if (t->newfile)
			return False;
	}
	if (st.st_ino != t->stat.st_ino ||
	    st.st_dev != t->stat.st_dev ||
	    st.st_mtime != t->stat.st_mtime ||
	    st.st_mtim.tv_nsec != t->stat.st_mtim.tv_nsec) {
		t->file_changed = 1;
		call("doc:notify:doc:status-changed", p);
		return True;
	}
	return False;
}

DEF_CMD(text_readonly)
{
	struct text *t = ci->home->doc_data;

	if (t->file_changed && !t->doc.readonly && ci->num)
		t->file_changed = 2;
	/* Use default handling */
	return Efallthrough;
}

static const char *safe autosave_name(const char *name safe)
{
	char *tempname = malloc(strlen(name) + 3 + 10);
	const char *base;
	char *tbase;

	strcpy(tempname, name);
	base = strrchr(name, '/');
	if (base)
		base += 1;
	else
		base = name;
	tbase = tempname + (base - name);
	sprintf(tbase, "#%s#", base);
	return tempname;
}

DEF_CMD(text_load_file)
{
	int fd = ci->num2;
	const char *name = ci->str;
	off_t size;
	struct text_alloc *a;
	struct text_chunk *c = NULL;
	int len;
	struct text *t = ci->home->doc_data;

	if (t->saved != t->undo)
		return Einval;
	if (fd < 0 && (ci->num & 6) && t->fname) {
		/* re-open existing file name */
		if (ci->num & 4)
			fd = open(t->autosave_name, O_RDONLY);
		else
			fd = open(t->fname, O_RDONLY);
		name = t->fname;
	}
	if (fd < 0) {
		size = 0;
		t->newfile = 1;
	} else {
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
		alloc(c, text);
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
		struct stat stb;

		if (fstat(fd, &t->stat) < 0) {
			t->newfile = 1;
			memset(&t->stat, 0, sizeof(t->stat));
		}
		if (name != t->fname) {
			const char *dname;
			free((void*)t->fname);
			t->fname = strdup(name);
			dname = strrchr(name, '/');
			if (dname)
				dname += 1;
			else
				dname = name;
			call("doc:set-name", ci->home, 0, NULL, dname);
		}
		if (!t->autosave_name)
			t->autosave_name = autosave_name(name);
		if (stat(t->autosave_name, &stb) == 0 &&
		    stb.st_mtime > t->stat.st_mtime)
			t->autosave_exists = True;
	}
	if (ci->num & 4) {
		/* restored from autoload, so nothing matches saved version */
		t->saved = (void*)1;
		t->file_changed = 2;
	} else {
		/* Current state is 'saved' */
		t->saved = t->undo;
		t->file_changed = 0;
	}
	call("doc:notify:doc:status-changed", ci->home);
	pane_notify("doc:replaced", ci->home);
	if (fd != ci->num2)
		close(fd);
	return 1;
err:
	unalloc(c, text);
	if (fd != ci->num2)
		close(fd);
	return Efallthrough;
}

DEF_CMD(text_insert_file)
{
	struct text *t = ci->home->doc_data;
	struct mark *pm = ci->mark, *early;
	struct text_alloc *a;
	int len;
	int fd = ci->num;
	off_t size, start;
	bool first = True;
	bool status_changes = (t->undo == t->saved);

	if (check_readonly(ci))
		return Efail;
	if (!pm || fd < 0 || fd == NO_NUMERIC)
		return Enoarg;
	size = lseek(fd, 0, SEEK_END);
	lseek(fd, 0, SEEK_SET);
	if (size < 0)
		return Efail;
	a = t->alloc;
	if (a->size - a->free < size)
		a = text_new_alloc(t, size);
	if (!a)
		return Efail;

	early = mark_dup(pm);
	mark_step(early, 0);

	start = a->free;
	while (a->free < start + size &&
	       (len = read(fd, a->text + a->free, start + size - a->free)) > 0)
		a->free += len;
	text_add_str(t, pm, a->text + start, size, &first);

	text_check_consistent(t);
	text_check_autosave(ci->home);
	if (status_changes)
		call("doc:notify:doc:status-changed", ci->home);
	pane_notify("doc:replaced", ci->home, 0, early, NULL,
		    0, pm);
	mark_free(early);

	return 1;
}

static bool do_text_output_file(struct pane *p safe, struct doc_ref *start,
				struct doc_ref *end, int fd)
{
	struct text *t = p->doc_data;
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
			return False;
		offset = 0;
		if (end && end->c == c)
			break;
	}
	if (fsync(fd) != 0)
		return False;
	return True;
}

static bool do_text_write_file(struct pane *p safe, struct doc_ref *start,
			       struct doc_ref *end,
			       const char *fname safe)
{
	/* Don't worry about links for now
	 * Create a temp file with #basename#~, write to that,
	 * copy mode across, fsync and then rename
	 */
	struct text *t = p->doc_data;
	char *tempname = malloc(strlen(fname) + 3 + 10);
	const char *base;
	char *tbase;
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
		return False;

	if (!do_text_output_file(p, start, end, fd))
		goto error;
	if (stat(fname, &stb) == 0 &&
	    S_ISREG(stb.st_mode))
		/* Preserve modes, but not setuid */
		fchmod(fd, stb.st_mode & 0777);
	if (fname == t->fname && check_file_changed(p)) {
		/* We are saving to a file which changed since we read it,
		 * so let's move that changed file to a backup
		 */
		int i;

		for (i = 1 ; i < 1000; i++) {
			char *new = NULL;
			if (asprintf(&new, "%s~%d~", fname, i) < 0)
				break;
			if (link(fname, new) == 0) {
				free(new);
				break;
			}
			free(new);
			if (errno != EEXIST)
				break;
		}
	}
	if (rename(tempname, fname) < 0)
		goto error;
	fstat(fd, &t->stat);
	close(fd);
	free(tempname);
	return True;
error:
	close(fd);
	unlink(tempname);
	free(tempname);
	return False;

}

static void autosaves_record(struct pane *p safe, const char *path safe,
			     bool create)
{
	DIR *d;
	char *home = getenv("HOME");
	char *dirname = getenv("EDLIB_AUTOSAVE");
	int num;
	bool changed = False;

	if (!home)
		home = "/tmp";
	if (!dirname)
		dirname = strconcat(p, home, "/.edlib_autosave");
	d = opendir(dirname);
	if (!d) {
		if (!create)
			return;
		if (mkdir(dirname, 0770) < 0)
			return;
		d = opendir(dirname);
		if (!d)
			return;
		num = 1;
	} else {
		struct dirent *de;

		num = 1;
		while ((de = readdir(d)) != NULL) {
			char *ep = NULL;
			long n;
			int len;
			char current[PATH_MAX];

			if (de->d_name[0] == '.')
				continue;
			n = strtoul(de->d_name, &ep, 10);
			if (!ep || ep == de->d_name || *ep != '\0')
				continue;
			if (n >= num)
				num = n + 1;
			len = readlinkat(dirfd(d), de->d_name,
					 current, sizeof(current));
			if (len <= 0 || len >= (int)sizeof(current))
				continue;
			current[len] = 0;
			if (strcmp(current, path) == 0) {
				if (!create) {
					unlinkat(dirfd(d), de->d_name, 0);
					changed = True;
				}
				create = False;
				break;
			}
		}
	}
	if (create) {
		char nbuf[20];
		snprintf(nbuf, sizeof(nbuf), "%d", num);
		symlinkat(path, dirfd(d), nbuf);
	}
	if (changed) {
		struct pane *doc;
		doc = call_ret(pane, "doc:open", p, -1, NULL, dirname);
		if (doc)
			pane_call(doc, "doc:notify:doc:revisit", p);
	}
	closedir(d);
}

static void do_text_autosave(struct pane *p safe)
{
	struct text *t = p->doc_data;
	int fd = -1;

	if (!t->fname)
		return;
	check_file_changed(p);

	if (!t->autosave_name)
		t->autosave_name = autosave_name(t->fname);
	if (t->as.changes == 0) {
		unlink(t->autosave_name);
		t->autosave_exists = False;
		autosaves_record(p, t->fname, False);
		return;
	}
	fd = open(t->autosave_name, O_WRONLY|O_CREAT|O_TRUNC, 0666);
	if (fd < 0)
		return;

	if (!do_text_output_file(p, NULL, NULL, fd)) {
		close(fd);
		unlink(t->autosave_name);
		return;
	}
	t->as.changes = 0;
	close(fd);
	autosaves_record(p, t->fname, True);
}

DEF_CMD(text_autosave_delete)
{
	struct pane *home = ci->home;
	struct text *t = home->doc_data;
	const char *name = ci->str;
	int ret = 1;

	if (!t->fname || !name)
		return Enoarg;

	if (!t->autosave_name)
		t->autosave_name = autosave_name(t->fname);

	if (strcmp(name, t->autosave_name) != 0 ||
	    unlink(t->autosave_name) < 0)
		ret = Efail;
	t->autosave_exists = False;
	autosaves_record(home, t->fname, False);

	return ret;
}

DEF_CMD(text_autosave_tick)
{
	struct pane *home = ci->home;
	struct text *t = home->doc_data;

	t->as.timer_started = 0;
	if (!t->fname)
		return Efalse;
	if (t->as.changes == 0)
		/* This will delete the file */
		do_text_autosave(home);
	if (time(NULL) - t->as.last_change >= 30)
		do_text_autosave(home);
	else {
		t->as.timer_started = 1;
		call_comm("event:timer", home, &text_autosave_tick,
			  (t->as.last_change + 30 - time(NULL)) * 1000);
	}
	return Efalse;
}

static void text_check_autosave(struct pane *p safe)
{
	struct text *t = p->doc_data;

	if (t->undo == t->saved)
		t->as.changes = 0;
	else
		t->as.changes += 1;
	t->as.last_change = time(NULL);
	if (!t->fname)
		return;
	if (t->as.changes > 300 || t->as.changes == 0)
		do_text_autosave(p);
	else if (!t->as.timer_started) {
		t->as.timer_started = 1;
		call_comm("event:timer", p, &text_autosave_tick,
			  30 * 1000);
	}
}

DEF_CMD(text_save_file)
{
	struct text *t = ci->home->doc_data;
	int ret;
	char *msg;
	int change_status = 0;

	if (!t->fname) {
		asprintf(&msg, "** No file name known for %s ***", t->doc.name);
		ret = Efail;
	} else {
		ret = do_text_write_file(ci->home, NULL, NULL, t->fname);
		if (ret) {
			asprintf(&msg, "Successfully wrote %s", t->fname);
			t->saved = t->undo;
			change_status = 1;
			t->file_changed = 0;
			t->newfile = 0;
		} else
			asprintf(&msg, "*** Failed to write %s ***", t->fname);
	}
	call("Message", ci->focus, 0, NULL, msg);
	free(msg);
	if (change_status)
		call("doc:notify:doc:status-changed", ci->home);
	text_check_autosave(ci->home);
	if (ret == 0)
		return 1;
	return Efail;
}

DEF_CMD(text_write_file)
{
	int ret;
	bool use_marks = ci->mark && ci->mark2;

	if (ci->str) {
		ret = do_text_write_file(ci->home,
					 use_marks ? &ci->mark->ref: NULL,
					 use_marks ? &ci->mark2->ref: NULL,
					 ci->str);
		return ret ? 1 : Efail;
	}
	if (ci->num >= 0 && ci->num != NO_NUMERIC) {
		ret = do_text_output_file(ci->home,
					  use_marks ? &ci->mark->ref: NULL,
					  use_marks ? &ci->mark2->ref: NULL,
					  ci->num);
		return ret ? 1 : Efail;
	}
	return Enoarg;
}

DEF_CMD(text_same_file)
{
	struct text *t = ci->home->doc_data;
	struct stat stb, stb2;
	int fd = ci->num2;

	if (t->fname == NULL)
		return Efallthrough;
	if (ci->str && strcmp(ci->str, t->fname) == 0)
		return 1;
	if (fd >= 0) {
		if (fstat(fd, &stb) != 0)
			return Efallthrough;
	} else if (ci->str) {
		if (stat(ci->str, &stb) != 0)
			return Efallthrough;
	} else
		return Efallthrough;
	if (t->stat.st_ino != stb.st_ino ||
	    t->stat.st_dev != stb.st_dev)
		return Efallthrough;
	/* Must check file hasn't changed beneath us */
	if (stat(t->fname, &stb2) != 0)
		stb2.st_ino = 0;
	if (stb2.st_ino == stb.st_ino &&
	    stb2.st_dev == stb.st_dev)
		return 1;
	return Efallthrough;
}

static void text_add_edit(struct text *t safe, struct text_chunk *target safe,
			  bool *first safe, int at_start, int len)
{
	struct text_edit *e;

	if (len == 0)
		return;

	if (t->saved == t->undo)
		/* Must never merge undo entries across a save point */
		*first = 1;

	if (t->redo) {
		/* Cannot add an edit before some redo edits, as they
		 * will get confused.  We need to record the redo history
		 * here in the undo history, possibly allocating
		 * a nop edit (len == 0)
		 */
		if (t->undo == NULL || t->undo->altnext != NULL) {
			alloc(e, undo);
			e->target = target; /* ignored */
			e->first = 0;
			e->at_start = 0;
			e->len = 0; /* This is a no-op */
			e->next = t->undo;
			t->undo = e;
		}
		t->undo->altnext = t->redo;
		t->undo->alt_is_second = 0;
		t->redo = NULL;
	}
	/* factoring out t->undo here avoids a bug in smatch. */
	e = t->undo;
	if (e && e->len != 0 && e->len + len != 0 && !*first &&
	    e->target == target && e->at_start == at_start) {
		/* This new edit can be merged with the previous one */
		e->len += len;
	} else {
		alloc(e, undo);
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
}

static void _text_add_str(struct text *t safe, struct doc_ref *pos safe,
			  const char *str safe, off_t len,
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
	off_t len2;
	off_t orig_len;

	if (len < 0)
		len = strlen(str);
	orig_len = len;
	if (start)
		*start = *pos;

	len2 = len;
	if (pos->c && pos->o == pos->c->end &&
	    pos->c->txt + pos->o == a->text + a->free &&
	    str != a->text + a->free &&
	    (a->size - a->free >= len ||
	     (len2 = utf8_round_len(str, a->size - a->free)) > 0)) {
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
		struct text_chunk *c;
		alloc(c, text);
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
			/* this implicitly truncates pos->c, so don't need
			 * to record that. */
		}
	}
	while (len > 0) {
		/* Make sure we have an empty chunk */
		if (pos->c->end > pos->c->start) {
			struct text_chunk *c;
			alloc(c, text);
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
		    (len2 = utf8_round_len(str, a->size - a->free)) == 0) {
			if (orig_len < 128 ||
			    t->alloc->size < DEFAULT_SIZE)
				a = text_new_alloc(t, DEFAULT_SIZE);
			else if (len > DEFAULT_SIZE && len > t->alloc->size)
				a = text_new_alloc(t, ((len +256) | 4095) + 1 - 256);
			else if (t->alloc->size * 2 < MAX_SIZE)
				a = text_new_alloc(t, t->alloc->size * 2);
			else
				a = text_new_alloc(t, MAX_SIZE);
			len2 = len;
			if (len2 > a->size)
				len2 = utf8_round_len(str, a->size);
		}
		pos->c->txt = a->text + a->free;
		pos->c->end = len2;
		pos->o = len2;
		if (str != pos->c->txt)
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

static int text_update_prior_after_change(struct text *t safe,
					  struct doc_ref *pos safe,
					  struct doc_ref *spos safe,
					  struct doc_ref *epos safe)
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

static int text_update_following_after_change(struct text *t safe,
					      struct doc_ref *pos safe,
					      struct doc_ref *spos safe,
					      struct doc_ref *epos safe)
{
	/* A change has happened between spos and epos. pos should be
	 * at or after epos.
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

static void text_del(struct text *t safe, struct doc_ref *pos safe,
		     unsigned int len, bool *first_edit safe)
{
	while (len) {
		struct text_chunk *c = pos->c;
		if (c == NULL)
			/* nothing more to delete */
			break;
		if (pos->o == c->start &&
		    len >= c->end - c->start) {
			/* The whole chunk is deleted, simply disconnect it */
			if (c != list_last_entry(&t->text,
						 struct text_chunk, lst)) {
				pos->c = list_next_entry(c, lst);
				pos->o = pos->c->start;
			} else if (c != list_first_entry(&t->text,
							 struct text_chunk,
							 lst)) {
				pos->c = list_prev_entry(c, lst);
				pos->o = pos->c->end;
			} else {
				/* Deleted final chunk */
				pos->c = NULL;
				pos->o = 0;
			}
			__list_del(c->lst.prev, c->lst.next); /* no poison,
							       * retain place
							       * in list */
			attr_free(&c->attrs);
			text_add_edit(t, c, first_edit, 0, c->start - c->end);
			len -= c->end - c->start;
			/* make sure undo knows this is empty at not attached */
			c->end = c->start;
		} else if (pos->o == c->start) {
			/* If the start of the chunk is deleted, just update.
			 * Note that len must be less that full size, else
			 * previous branch would have been taken.
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
			if (len && c != list_last_entry(&t->text,
							struct text_chunk,
							lst)) {
				pos->c = list_next_entry(c, lst);
				pos->o = pos->c->start;
			} else
				len = 0;
		} else {
			/* must be deleting out of the middle of the chunk.
			 * need to create new chunk for the 'after' bit.
			 */
			struct text_chunk *c2;
			alloc(c2, text);
			c2->txt = c->txt;
			c2->start = pos->o + len;
			c2->end = c->end;
			c->end = pos->o;
			c2->attrs = attr_copy_tail(c->attrs, c2->start);
			attr_trim(&c->attrs, c->end);
			list_add(&c2->lst, &c->lst);
			/* This implicitly trims c, so we only have len
			 * left to trim */
			text_add_edit(t, c2, first_edit, 0,
				      c2->end - c2->start);
			text_add_edit(t, c, first_edit, 0, -len);
			len = 0;
		}
	}
}

/* text_undo and text_redo:
 *
 * The 'start' and 'end' reported identify the range changed.  For a reversed
 * insertion they will be the same.  If the undo results in the buffer being
 * empty, both start and end will point to a NULL chunk.
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
		if (e->target == list_last_entry(&t->text,
						 struct text_chunk, lst)) {
			end->c = NULL;
			end->o = 0;
		} else {
			end->c = list_next_entry(e->target, lst);
			end->o = end->c->start;
		}
		*start = *end;

		__list_del(e->target->lst.prev, e->target->lst.next);
		/* If this was created for a split, we need to extend the
		 * other half
		 */
		if (e->target != list_first_entry(&t->text,
						  struct text_chunk, lst)) {
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
		if (e->target != list_first_entry(&t->text,
						  struct text_chunk, lst)) {
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
		/* This chunk is deleted, so leave start/end pointing
		 * beyond it */
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

static bool check_readonly(const struct cmd_info *ci safe)
{
	struct text *t = ci->home->doc_data;

	if (t->undo == t->saved &&
	    check_file_changed(ci->home) &&
	    !t->doc.readonly) {
		call("doc:notify:doc:status-changed", ci->home);
		t->doc.readonly = 1;
	}
	if (!t->doc.readonly)
		return False;
	call("Message", ci->focus, 0, NULL, "Document is read-only");
	return True;
}

DEF_CMD(text_reundo)
{
	struct mark *m = ci->mark;
	struct doc_ref start, end;
	int last = 0;
	struct text_edit *ed = NULL;
	bool first = 1;
	int status;
	struct text *t = ci->home->doc_data;

	if (!m)
		return Enoarg;

	if (check_readonly(ci))
		return Efail;

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
			mark_reset(ci->home, m, 0);
			where = 1;
			first = 0;
		}

		t->revising_marks = True;
		if (where == 1) {
			mark_step(m, 1);
			do {
				struct doc_ref tmp = m->ref;
				i = text_advance_towards(t, &tmp, &end);
				if (i == 0)
					break;
				while ((m2 = mark_next(m)) != NULL &&
				       m2->ref.c == tmp.c &&
				       m2->ref.o <= tmp.o)
					mark_to_mark_noref(m, m2);
				m->ref = tmp;
			} while (i == 2);
		} else {
			mark_step(m, 0);
			do {
				struct doc_ref tmp = m->ref;
				i = text_retreat_towards(t, &tmp, &end);
				if (i == 0)
					break;
				while ((m2 = mark_prev(m)) != NULL &&
				       m2->ref.c == tmp.c &&
				       m2->ref.o >= tmp.o)
					mark_to_mark_noref(m, m2);
				m->ref = tmp;
			} while (i == 2);
		}
		t->revising_marks = False;

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
							       &start,
							       &end) == 0)
				break;

		text_normalize(t, &m->ref);
		if (text_ref_same(t, &start, &end))
			early = m;
		else {
			early = mark_dup(m);
			mark_step(early, 0);
			/* There cannot be any mark between start and end,
			 * so it is safe to assign 'ref' here.
			 */
			early->ref = start;
		}
		pane_notify("doc:replaced", ci->home,
			    0, early, NULL,
			    0, m);
		if (early != m)
			mark_free(early);

		text_check_consistent(t);

	} while (ed && !last);

	text_check_consistent(t);

	if (status != (t->undo == t->saved))
		call("doc:notify:doc:status-changed", ci->home);
	text_check_autosave(ci->home);

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
	/* Adjust so at not at the end of a chunk - either ->o points
	 * at a byte, or ->c is NULL.
	 */
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
	/* Ensure r->o is after some byte, or at start of file. */
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

static void text_add_str(struct text *t safe, struct mark *pm safe,
			 const char *str safe, off_t size, bool *first safe)
{
	struct doc_ref start;
	struct mark *m;

	text_denormalize(t, &pm->ref);
	_text_add_str(t, &pm->ref, str, size, &start, first);
	text_normalize(t, &pm->ref);
	for (m = mark_prev(pm);
	     m && text_update_prior_after_change(t, &m->ref,
						 &start, &pm->ref);
	     m = mark_prev(m))
		;
	for (m = mark_next(pm);
	     m && text_update_following_after_change(t, &m->ref,
						     &start, &pm->ref);
	     m = mark_next(m))
		;
}

static inline wint_t text_next(struct pane *p safe, struct doc_ref *r safe, bool bytes)
{
	struct text *t = p->doc_data;
	wint_t ret = WERR;
	const char *c;

	text_normalize(t, r);
	if (r->c == NULL)
		return WEOF;

	c = r->c->txt + r->o;
	if (!bytes)
		ret = get_utf8(&c, r->c->txt + r->c->end);
	if (ret < WERR)
		r->o = c - r->c->txt;
	else
		ret = (unsigned char)r->c->txt[r->o++];
	text_normalize(t, r);
	return ret;
}

static inline wint_t text_prev(struct pane *p safe, struct doc_ref *r safe, bool bytes)
{
	struct text *t = p->doc_data;
	wint_t ret;
	const char *c;

	text_denormalize(t, r);
	if (list_empty(&t->text))
		return WEOF;
	if (r->c == NULL || r->o <= r->c->start)
		// assert (r->c->lst.prev == &t->text)
		return WEOF;

	if (bytes)
		r->o -= 1;
	else {
		r->o = r->c->start +
			utf8_round_len(r->c->txt+r->c->start,
				       r->o - r->c->start - 1);
		c = r->c->txt + r->o;
		ret = get_utf8(&c, r->c->txt + r->c->end);
		if (ret < WERR)
			return ret;
	}

	ret = (unsigned char)r->c->txt[r->o];
	return ret;
}

DEF_CMD(text_char_byte)
{
	return do_char_byte(ci);
}
static bool _text_ref_same(struct text *t safe, struct doc_ref *r1 safe,
			   struct doc_ref *r2 safe)
{
	if (r1->c == r2->c) {
		return r1->o == r2->o;
	}
	if (r1->c == NULL /*FIXME redundant*/ && r2->c != NULL) {
		if (list_empty(&t->text))
			return True;
		return (r2->o == r2->c->end &&
			r2->c->lst.next == &t->text);
	}
	if (r2->c == NULL /* FIXME redundant*/ && r1->c != NULL) {
		if (list_empty(&t->text))
			return True;
		return (r1->o == r1->c->end &&
			r1->c->lst.next == &t->text);
	}
	/* FIXME impossible */
	if (r1->c == NULL || r2->c == NULL) return False;

	if (r1->o == r1->c->end &&
	    r2->o == r2->c->start &&
	    list_next_entry(r1->c, lst) == r2->c)
		return True;
	if (r1->o == r1->c->start &&
	    r2->o == r2->c->end &&
	    list_prev_entry(r1->c, lst) == r2->c)
		return True;
	return False;
}

static bool text_ref_same(struct text *t safe, struct doc_ref *r1 safe,
			 struct doc_ref *r2 safe)
{
	bool ret = _text_ref_same(t, r1, r2);
	ASSERT(ret == (r1->c == r2->c && r1->o == r2->o));
	return ret;
}

DEF_LOOKUP_CMD(text_handle, text_map);

DEF_CMD(text_new)
{
	struct text *t;
	struct pane *p;

	p = doc_register(ci->home, &text_handle.c);
	if (!p)
		return Efail;
	t = p->doc_data;
	t->alloc = safe_cast NULL;
	INIT_LIST_HEAD(&t->text);
	t->saved = t->undo = t->redo = NULL;
	t->prev_edit = Redo;
	t->fname = NULL;
	t->file_changed = 0;
	t->stat.st_ino = 0;
	t->as.changes = 0;
	t->as.timer_started = 0;
	t->as.last_change = 0;
	text_new_alloc(t, 0);

	return comm_call(ci->comm2, "callback:doc", p);
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

DEF_CMD(text_content)
{
	struct mark *from = ci->mark, *to = ci->mark2;
	struct mark *m;
	struct text *t = ci->home->doc_data;
	struct text_chunk *c, *first, *last;
	int bytes = strcmp(ci->key, "doc:content-bytes") == 0;
	int l = 0, head, tail;
	int size = 0;

	if (!from)
		return Enoarg;
	m = mark_dup(from);
	head = 0;
	first = from->ref.c;
	if (first)
		head = from->ref.o - first->start;
	last = NULL;
	tail = 0;
	if (to) {
		/* Calculate size so comm2 can pre-allocate */
		if (to->ref.c) {
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
		size = l;
	}
	l = 0;
	c = first;
	list_for_each_entry_from(c, &t->text, lst) {
		struct mark *m2;
		const char *s = c->txt + c->start;
		int ln = c->end - c->start;
		if (c == first) {
			s += head;
			ln -= head;
		}
		if (c == last)
			ln -= tail;
		if (m->ref.c != c) {
			while ((m2 = mark_next(m)) &&
			       m2->ref.c == m->ref.c)
				mark_to_mark(m, m2);
			m->ref.c = c;
			m->ref.o = c->start;
		}
		while (ln > 0) {
			int rv;
			const char *ss = s;
			wint_t wc;

			if (bytes)
				wc = *s++;
			else {
				wc = get_utf8(&s, s+ln);
				if (wc >= WERR)
					wc = *s++;
			}

			while ((m2 = mark_next(m)) &&
			       m2->ref.c == m->ref.c &&
			       m2->ref.o <= s - c->txt)
				mark_to_mark(m, m2);
			m->ref.o = s - c->txt;
			text_normalize(t, &m->ref);

			ln -= s - ss;
			/* Interpreted can see " unterminated" and know
			 * than ->num2 is the length of ->str
			 */
			rv = comm_call(ci->comm2, "consume unterminated",
				       ci->focus,
				       wc, m, s, ln, NULL, NULL, size, 0);
			size = 0;
			if (rv <= 0 || rv > ln + 1) {
				/* Time to stop */
				ln = 0;
				c = last;
			} else if (rv > 1) {
				/* consumed (some of) str */
				s += rv - 1;
				ln -= rv - 1;
			}
		}
		if (c == last)
			break;
	}
	mark_free(m);
	return 1;
}

DEF_CMD(text_debug_mark)
{
	char *ret = NULL;
	struct text_chunk *c;
	struct mark *m = ci->mark;

	if (!m || m->owner != ci->home || !ci->comm2)
		return Enoarg;
	c = m->ref.c;
	if (!mark_valid(m))
		ret = strdup("M:FREED");
	else if (!c)
		ret = strdup("M:EOF");
	else {
		unsigned int len = c->end - c->start;
		unsigned int o = ci->mark->ref.o;

		if (o <= c->start + 4 || len <= 8) {
			if (len > 8)
				len = 8;
			asprintf(&ret, "M:(%.*s[%d])", len,
				 c->txt + c->start, m->ref.o);
		} else {
			len = c->end - m->ref.o;
			if (len > 4)
				len = 4;
			asprintf(&ret, "M:(%.4s..[%d]%.*s)",
				 c->txt + c->start,
				 m->ref.o, len, c->txt + m->ref.o);
		}
	}
	comm_call(ci->comm2, "cb", ci->focus, 0, NULL, ret);
	free(ret);
	return 1;
}

DEF_CMD(text_val_marks)
{
	struct text *t = ci->home->doc_data;
	struct text_chunk *c;
	int found;

	if (!ci->mark || !ci->mark2)
		return Enoarg;

	if (t->revising_marks)
		return 1;

	if (ci->mark->ref.c == ci->mark2->ref.c) {
		if (ci->mark->ref.o < ci->mark2->ref.o)
			return 1;
		LOG("text_val_marks: same buf, bad offset: %u, %u",
		    ci->mark->ref.o, ci->mark2->ref.o);
		return Efalse;
	}
	found = 0;
	list_for_each_entry(c, &t->text, lst) {
		if (ci->mark->ref.c == c)
			found = 1;
		if (ci->mark2->ref.c == c) {
			if (found == 1)
				return 1;
			LOG("text_val_marks: mark2.c found before mark1");
			return Efalse;
		}
	}
	if (ci->mark2->ref.c == NULL) {
		if (found == 1)
			return 1;
		LOG("text_val_marks: mark2.c (NULL) found before mark1");
		return Efalse;
	}
	if (found == 0)
		LOG("text_val_marks: Neither mark found in chunk list");
	if (found == 1)
		LOG("text_val_marks: mark2 not found in chunk list");
	return Efalse;
}

DEF_CMD(text_set_ref)
{
	struct mark *m = ci->mark;
	struct text *t = ci->home->doc_data;

	if (!m)
		return Enoarg;
	mark_to_end(ci->home, m, ci->num != 1);
	if (list_empty(&t->text) || ci->num != 1) {
		m->ref.c = NULL;
		m->ref.o = 0;
	} else {
		m->ref.c = list_first_entry(&t->text, struct text_chunk, lst);
		m->ref.o = m->ref.c->start;
	}
	return 1;
}

static int text_advance_towards(struct text *t safe,
				struct doc_ref *ref safe,
				struct doc_ref *target safe)
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

	if (ref->c != target->c && (!ref->c || ref->o <= ref->c->start))
		if (text_prev(safe_cast container_of(t, struct pane, doc_data[0]), ref, 1) == WEOF)
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

static int text_locate(struct text *t safe, struct doc_ref *r safe,
		       struct doc_ref *dest safe)
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

	next = (next == NULL || next->lst.next == &t->text) ?
		NULL : list_next_entry(next, lst);
	prev = (prev == NULL || prev->lst.prev == &t->text) ?
		NULL : list_prev_entry(prev, lst);
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

static void text_ref_consistent(struct text *t safe, struct doc_ref *r safe,
				int *loops safe)
{
	struct text_chunk *c;

	if (r->c == NULL) {
		if (r->o)
			abort();
		return;
	}
	if (r->o >= r->c->end)
		abort();
	if (r->o < r->c->start)
		abort();
	list_for_each_entry(c, &t->text, lst) {
		if (r->c == c || *loops <= 0)
			return;
		(*loops) -= 1;
	}
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
	int loops = 10000;

	if (pane_no_consistency(safe_cast container_of(d, struct pane, doc)))
		return;

	list_for_each_entry(c, &t->text, lst) {
		check_allocated(t, c->txt, c->end);
		if (c->start >= c->end)
			abort();
		if (loops-- < 0)
			break;
	}
	list_for_each_entry(c, &t->text, lst) {
		struct text_chunk *c2;
		list_for_each_entry(c2, &t->text, lst) {
			if (loops -- < 0)
				break;
			if (c2 == c ||
			    c2->txt != c->txt)
				continue;
			if (c->start >= c2->end)
				continue;
			if (c2->start >= c->end)
				continue;
			abort();
		}
		if (loops-- < 0)
			break;
	}

	/* This test is quadratic in the number of marks, so let's
	 * give up rather then annoy the users.
	 */
	for (m = mark_first(d); m; m = mark_next(m))
		text_ref_consistent(t, &m->ref, &loops);

	prev = NULL;
	for (m = mark_first(d); m; m = mark_next(m)) {
		if (prev) {
			struct doc_ref r = prev->ref;/* SMATCH Bug things prev
						      * has no state*/
			int i;
			struct doc_ref r2 = m->ref;
			text_normalize(t, &r2);
			while ((i = text_advance_towards(t, &r,
							 &r2)) != 1) {
				if (i == 0)
					abort();
			}
		}
		prev = m;
		if (loops-- < 0)
			break;
	}
	doc_check_consistent(d);
}

static void text_add_attrs(struct attrset **attrs safe,
			   const char *new safe, int o)
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

	struct text *t = ci->home->doc_data;
	struct mark *pm = ci->mark2;
	struct mark *end = ci->mark;
	const char *str = ci->str;
	const char *newattrs = ci->str2;
	bool first = !ci->num2;
	struct mark *early = NULL;
	int status_change = 0;

	if (check_readonly(ci))
		return Efail;

	if (!pm) {
		/* Default to insert at end */
		pm = point_new(ci->home);
		if (!pm)
			return Efail;
		mark_reset(ci->home, pm, 1);
	}

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
		/* pm is at the start, myend is at the end */
		l = count_bytes(t, pm, myend);
		mark_free(myend);
		text_del(t, &pm->ref, l, &first);
		text_normalize(t, &pm->ref);

		for (m = mark_prev(pm);
		     m && text_update_prior_after_change(t, &m->ref,
							 &pm->ref, &pm->ref);
		     m = mark_prev(m))
			;
		for (m = mark_next(pm);
		     m && text_update_following_after_change(t, &m->ref,
							     &pm->ref,
							     &pm->ref);
		     m = mark_next(m))
			;
		text_check_consistent(t);
	}
	if (end && end != pm)
		early = end;
	else
		early = mark_dup(pm);
	/* leave "early" at the start of the insertion, and
	 * pm moves to the end - they are both currently at
	 * the same location in the doc.
	 */
	mark_step(early, 0);

	if (str && *str) {
		if (t->undo == t->saved)
			status_change = 1;

		text_add_str(t, pm, str, -1, &first);
		if (newattrs && early->ref.c)
			text_add_attrs(&early->ref.c->attrs, newattrs,
				       early->ref.o);
		text_check_consistent(t);

	}
	text_check_autosave(ci->home);
	if (status_change)
		call("doc:notify:doc:status-changed", ci->home);
	pane_notify("doc:replaced", ci->home, 0, early, NULL,
		    0, pm);
	if (early != end)
		mark_free(early);
	if (!ci->mark2)
		mark_free(pm);
	return first ? 1 : 2;
}

static struct attrset *text_attrset(struct pane *p safe, struct mark *m safe,
				    int *op safe)
{
	struct text_chunk *c;
	struct text *t = p->doc_data;
	unsigned int o;

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
	struct mark *m = ci->mark;
	const char *attr = ci->str;
	const char *val;
	struct attrset *a;
	int o = 0;

	if (!m || !attr)
		return Enoarg;
	a = text_attrset(ci->home, m, &o);
	val = attr_get_str(a, attr, o);
	if (!val && !ci->num2)
		return Efallthrough;
	comm_call(ci->comm2, "callback:get_attr", ci->focus, 0, m, val,
		  0, NULL, attr);
	if (ci->num2 == 1) {
		const char *key = attr;
		int len = strlen(attr);
		while ((key = attr_get_next_key(a, key, o, &val)) != NULL &&
		       strncmp(key, attr, len) == 0)
			comm_call(ci->comm2, "callback:get_attr", ci->focus,
				  0, m, val, 0,
				  NULL, key);
	}
	return 1;
}

DEF_CMD(text_get_attr)
{
	struct text *t = ci->home->doc_data;
	const char *attr = ci->str;
	const char *val;

	if (!attr)
		return Enoarg;

	if ((val = attr_find(ci->home->attrs, attr)) != NULL)
		;
	else if (strcmp(attr, "render-default") == 0)
		val = "text";
	else if (strcmp(attr, "doc-type") == 0)
		val = "text";
	else if (strcmp(attr, "doc:charset") == 0)
		val = "utf-8";
	else if (strcmp(attr, "filename") == 0)
		val = t->fname;
	else if (strcmp(attr, "doc-file-changed") == 0)
		val = t->file_changed ? "yes" : "no";
	else if (strcmp(attr, "doc-modified") == 0)
		val = (t->saved != t->undo) ? "yes" : "no";
	else if (strcmp(attr, "autosave-exists") == 0)
		val = t->autosave_exists ? "yes" : "no";
	else if (strcmp(attr, "autosave-name") == 0) {
		if (!t->autosave_name && t->fname)
			t->autosave_name = autosave_name(t->fname);
		val = t->autosave_name;
	} else if (strcmp(attr, "is_backup") == 0) {
		const char *f = t->fname ?: "";
		const char *base = strrchr(f, '/');
		int l;

		if (base)
			base += 1;
		else
			base = f;
		l = strlen(base);
		if (base[0] == '#' && base[l-1] == '#')
			val = "yes";
		else if (base[l-1] == '~' && strchr(base, '~') - base < l-1)
			val = "yes";
		else
			val = "no";
	} else if (strcmp(attr, "base-name") == 0) {
		char *f = strsave(ci->focus, t->fname ?: "");
		char *base;
		int l;

		if (!f)
			return Efail;
		base = strrchr(f, '/');
		if (base)
			base += 1;
		else
			base = f;
		l = strlen(base);
		val = f;
		if (base[0] == '#' && base[l-1] == '#') {
			base[l-1] = '\0';
			strcpy(base, base+1);
		} else if (base[l-1] == '~' && strchr(base, '~') - base < l-1) {
			while (l > 1 && base[l-2] != '~')
				l -= 1;
			base[l-2] = '\0';
		} else
			val = NULL;
	} else
		return Efallthrough;

	comm_call(ci->comm2, "callback:get_attr", ci->focus, 0, NULL, val);
	return 1;
}

DEF_CMD(text_set_attr)
{
	const char *attr = ci->str;
	const char *val = ci->str2;
	struct text_chunk *c, *c2;
	struct text *t = ci->home->doc_data;
	unsigned int o, o2;

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
	pane_notify("doc:replaced-attr", ci->home, 1, ci->mark, NULL,
		    0, ci->mark2);
	attr_set_str_key(&c->attrs, attr, val, o);
	if (!ci->mark2 || ci->mark2->seq <= ci->mark->seq)
		return Efallthrough;
	/* Delete all subsequent instances of attr */
	o += 1;
	o2 = ci->mark2->ref.o;
	c2 = ci->mark2->ref.c;
	while (c != c2) {
		attr_del_all(&c->attrs, attr, o, c->end);
		c = list_next_entry(c, lst);
		o = c ? c->start : 0;
	}
	if (c && o < o2)
		attr_del_all(&c->attrs, attr, o, o2);
	return Efallthrough;
}

DEF_CMD(text_modified)
{
	struct text *t = ci->home->doc_data;

	if (ci->num == 0) {
		/* toggle status */
		if (t->saved == t->undo)
			t->saved = NULL;
		else
			t->saved = t->undo;
	} else if (ci->num > 0)
		/* Set "is modified" */
		t->saved = NULL;
	else
		/* Clear "is modified" */
		t->saved = t->undo;
	text_check_autosave(ci->home);
	call("doc:notify:doc:status-changed", ci->home);
	return 1;
}

DEF_CMD(text_revisited)
{
	struct text *t = ci->home->doc_data;

	if (ci->num <= 0)
		/* Being buried, not visited */
		return Efallthrough;

	if (check_file_changed(ci->home) && t->saved == t->undo) {
		call("doc:load-file", ci->home, 2, NULL, NULL, -1);
		call("Message", ci->focus, 0, NULL, "File Reloaded");
	}
	return Efallthrough;
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
		struct text_chunk *c = list_entry(t->text.next,
						  struct text_chunk, lst);
		list_del(&c->lst);
		attr_free(&c->attrs);
		unalloc(c, text);
	}
	ta = t->alloc;
	while (ta) {
		struct text_alloc *tmp = ta;
		ta = tmp->prev;
		unalloc_buf(tmp, sizeof(*tmp) + tmp->size, text);
	}
	t->alloc = safe_cast NULL;
	while (t->undo) {
		struct text_edit *te = t->undo;

		if (te->altnext == NULL) {
			t->undo = te->next;
			unalloc(te, undo);
		} else if (te->next == NULL) {
			t->undo = te->altnext;
			unalloc(te, undo);
		} else {
			/* Make the ->altnext link shorted, until it
			 * disappears
			 */
			t->undo = te->altnext;
			te->altnext = t->undo->next;
			t->undo->next = te;
		}
	}
	while (t->redo) {
		struct text_edit *te = t->redo;

		if (te->altnext == NULL) {
			t->redo = te->next;
			unalloc(te, undo);
		} else if (te->next == NULL) {
			t->redo = te->altnext;
			unalloc(te, undo);
		} else {
			/* Make the ->altnext link shorted, until it
			 * disappears
			 */
			t->redo = te->altnext;
			te->altnext = t->redo->next;
			t->redo->next = te;
		}
	}
}

DEF_CMD_CLOSED(text_destroy)
{
	struct text *t = ci->home->doc_data;

	text_cleanout(t);
	free((void*)t->fname);
	t->fname = NULL;
	free((void*)t->autosave_name);
	t->autosave_name = NULL;
	return Efallthrough;
}

DEF_CMD(text_clear)
{
	/* Clear the document, including undo/redo records
	 * i.e. free all text
	 */
	struct text *t = ci->home->doc_data;
	struct mark *m;

	text_cleanout(t);
	text_new_alloc(t, 0);

	hlist_for_each_entry(m, &t->doc.marks, all) {
		m->ref.c = NULL;
		m->ref.o = 0;
	}
	pane_notify("doc:replaced", ci->home);

	return 1;
}

void edlib_init(struct pane *ed safe)
{
	call_comm("global-set-command", ed, &text_new, 0, NULL,
		  "attach-doc-text");
	call_comm("global-set-command", ed, &text_new2, 0, NULL,
		  "open-doc-text");

	text_map = key_alloc();

	key_add_chain(text_map, doc_default_cmd);
	key_add(text_map, "doc:load-file", &text_load_file);
	key_add(text_map, "doc:insert-file", &text_insert_file);
	key_add(text_map, "doc:same-file", &text_same_file);
	key_add(text_map, "doc:content", &text_content);
	key_add(text_map, "doc:content-bytes", &text_content);
	key_add(text_map, "doc:set-ref", &text_set_ref);
	key_add(text_map, "doc:save-file", &text_save_file);
	key_add(text_map, "doc:write-file", &text_write_file);
	key_add(text_map, "doc:reundo", &text_reundo);
	key_add(text_map, "doc:set-attr", &text_set_attr);
	key_add(text_map, "doc:get-attr", &text_doc_get_attr);
	key_add(text_map, "doc:replace", &text_replace);
	key_add(text_map, "doc:char", &text_char_byte);
	key_add(text_map, "doc:byte", &text_char_byte);
	key_add(text_map, "doc:modified", &text_modified);
	key_add(text_map, "doc:set:readonly", &text_readonly);
	key_add(text_map, "doc:notify:doc:revisit", &text_revisited);
	key_add(text_map, "doc:clear", &text_clear);
	key_add(text_map, "doc:autosave-delete", &text_autosave_delete);
	key_add(text_map, "doc:debug:mark", &text_debug_mark);
	key_add(text_map, "debug:validate-marks", &text_val_marks);

	key_add(text_map, "Close", &text_destroy);
	key_add(text_map, "get-attr", &text_get_attr);
}
