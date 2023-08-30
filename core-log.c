/*
 * Copyright Neil Brown ©2020-2023 <neil@brown.name>
 * May be distributed under terms of GPLv2 - see file:COPYING
 *
 * Logging support.
 *
 * Provide log() and related functions to collect trace data.
 * Store it in a buffer accessible as a document, and optionally
 * write to a file or stderr.
 *
 */

#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <sys/time.h>

#define PRIVATE_DOC_REF
struct doc_ref {
	struct logbuf *b;
	unsigned int o;
};
#define DOC_DATA_TYPE struct log
#define DOC_NEXT(p,m,r,b) log_next(p,r,b)
#define DOC_PREV(p,m,r,b) log_prev(p,r,b)

#include "core.h"
#include "internal.h"

struct logbuf {
	struct list_head h;
	unsigned int	end;
	char		text[];
};

static struct log {
	struct doc		doc;
	struct list_head	log;
	int blocked;
} *log_doc safe;

#include "core-pane.h"

static struct pane *log_pane;

#define LBSIZE (8192 - sizeof(struct logbuf))

static FILE *log_file;

static struct logbuf *safe get_new_buf(struct log *d safe)
{
	struct logbuf *b = malloc(sizeof(*b) + LBSIZE);

	list_add_tail(&b->h, &d->log);
	b->end = 0;
	return b;
}

static struct logbuf *safe get_buf(struct log *d safe)
{
	if (!list_empty(&d->log)) {
		struct logbuf *b;
		b = list_last_entry(&d->log, struct logbuf, h);
		if (b->end < LBSIZE)
			return b;
	}
	return get_new_buf(d);
}

void LOG(char *fmt, ...)
{
	va_list ap;
	unsigned int n;
	struct logbuf *b;
	struct timeval now;

	if (!(void*)log_doc)
		/* too early */
		return;
	if (!fmt)
		return;
	if (log_doc->blocked)
		return;
	log_doc->blocked = 1;
	gettimeofday(&now, NULL);
	b = get_buf(log_doc);
	va_start(ap, fmt);
	if (log_pane && edlib_testing(log_pane))
		n = 0;
	else
		n = snprintf(b->text + b->end, LBSIZE - b->end - 1,
			     "%ld.%03ld:",
			     now.tv_sec % 10000, now.tv_usec / 1000);
	if (n < LBSIZE - b->end - 1)
		n += vsnprintf(b->text + b->end + n, LBSIZE - b->end - 1 - n,
			       fmt, ap);
	va_end(ap);

	if (b->end != 0 && n >= LBSIZE - b->end - 1) {
		/* Didn't fit, allocate new buf */
		b = get_new_buf(log_doc);
		va_start(ap, fmt);
		if (log_pane && edlib_testing(log_pane))
			n = 0;
		else
			n = snprintf(b->text, LBSIZE - 1, "%ld.%03ld:",
				     now.tv_sec % 10000,
				     now.tv_usec / 1000);
		if (n < LBSIZE - 1)
			n += vsnprintf(b->text + n, LBSIZE - 1 - n, fmt, ap);
		va_end(ap);
	}
	if (n >= LBSIZE - 1) {
		/* Too long for buffer - truncate */
		n = LBSIZE - 2;
	}
	b->text[b->end + n++] = '\n';
	b->text[b->end + n] = '\0';

	if (log_file) {
		fwrite(b->text + b->end, 1, n, log_file);
		fflush(log_file);
	}
	b->end += n;
	if (log_pane)
		pane_notify("doc:replaced", log_pane, 1);
	log_doc->blocked = 0;
}

DEF_CMD(log_append)
{
	struct log *l = &ci->home->doc_data;
	struct logbuf *b;
	unsigned int len;

	if (!ci->str)
		return Enoarg;
	len = strlen(ci->str);

	b = get_buf(l);

	if (b->end != 0 && len >= LBSIZE - b->end - 1) {
		/* Doesn't fit, allocate new buf */
		b = get_new_buf(l);
		if (len >= LBSIZE-1)
			len = LBSIZE-2;
	}
	strncpy(b->text + b->end, ci->str, len);

	b->text[b->end + len++] = '\n';
	b->text[b->end + len] = '\0';

	b->end += len;
	pane_notify("doc:replaced", ci->home, 1);
	return 1;
}

DEF_CMD(log_content)
{
	struct log *log = &ci->home->doc_data;
	struct mark *from = ci->mark, *to = ci->mark2;
	struct mark *m;
	struct logbuf *b, *first, *last;
	int head, tail;
	int size = 0;
	int bytes = strcmp(ci->key, "doc:content-bytes") == 0;

	if (!from)
		return Enoarg;
	m = mark_dup(from);
	head = 0;
	first = from->ref.b;
	if (first)
		head = from->ref.o;
	last = NULL;
	tail = 0;
	if (to) {
		if (to->ref.b) {
			last = to->ref.b;
			tail = to->ref.o;
		}

		b = first;
		list_for_each_entry_from(b, &log->log, h) {
			if (b == last)
				break;
			size += b->end;
		}
		size += tail - head;
	}

	b = first;
	list_for_each_entry_from(b, &log->log, h) {
		struct mark *m2;
		const char *s = b->text + head;
		int ln = b->end - head;

		if (b == last)
			ln = tail - head;

		if (m->ref.b != b) {
			while ((m2 = mark_next(m)) &&
			       m2->ref.b == m->ref.b)
				mark_to_mark(m, m2);
			m->ref.b = b;
			m->ref.o = 0;
		}
		while (ln > 0) {
			int rv;
			const char *ss = s;
			wint_t wc;

			if (bytes)
				wc = *s++;
			else
				wc = get_utf8(&s, s+ln);
			if (wc >= WERR)
				break;

			while ((m2 = mark_next(m)) &&
			       m2->ref.b == m->ref.b &&
			       m2->ref.o <= s - b->text)
				mark_to_mark(m, m2);
			m->ref.o = s - b->text;

			ln -= s - ss;
			rv = comm_call(ci->comm2, "consume", ci->focus,
				       wc, m, s, ln, NULL, NULL, size, 0);
			size = 0;
			if (rv <= 0 || rv > ln + 1) {
				ln = 0;
				b = last;
			}
			if (rv > 1) {
				s += rv - 1;
				ln -= rv - 1;
			}
		}
		head = 0;
		if (b == last)
			break;
	}
	mark_free(m);
	return 1;
}

DEF_CMD(log_set_ref)
{
	struct log *log = &ci->home->doc_data;
	struct mark *m = ci->mark;

	if (!m)
		return Enoarg;
	mark_to_end(ci->home, m, ci->num != 1);
	m->ref.o = 0;
	if (ci->num == 1)
		m->ref.b = list_first_entry_or_null(&log->log, struct logbuf, h);
	else
		m->ref.b = NULL;
	return 1;
}

static inline wint_t log_next(struct pane *p safe, struct doc_ref *r safe, bool bytes)
{
	struct log *log = &p->doc_data;
	wint_t ret;

	if (!r->b)
		ret = WEOF;
	else {
		const char *s = &r->b->text[r->o];

		if (bytes)
			ret = *s++;
		else
			ret = get_utf8(&s, r->b->text + r->b->end);
		r->o = s - r->b->text;
		if (r->o >= r->b->end) {
			if (r->b == list_last_entry(&log->log,
						     struct logbuf, h))
				r->b = NULL;
			else
				r->b = list_next_entry(r->b, h);
			r->o = 0;
		}
	}
	return ret;
}

static inline wint_t log_prev(struct pane *p safe, struct doc_ref *r safe, bool bytes)
{
	struct log *log = &p->doc_data;
	const char *s;

	if (list_empty(&log->log))
		return WEOF;
	else if (!r->b) {
		r->b = list_last_entry(&log->log, struct logbuf, h);
		r->o = r->b->end;
	} else if (r->o == 0) {
		if (r->b != list_first_entry(&log->log,
					      struct logbuf, h)) {
			r->b = list_prev_entry(r->b, h);
			r->o = r->b->end;
		} else
			return WEOF;
	}
	if (bytes)
		r->o -= 1;
	else
		r->o = utf8_round_len(r->b->text, r->o - 1);
	s = r->b->text + r->o;
	if (bytes)
		return *s;
	else
		return get_utf8(&s, r->b->text + r->b->end);
}

DEF_CMD(log_char)
{
	return do_char_byte(ci);
}

DEF_CMD(log_val_marks)
{
	/* mark1 and mark2 must be correctly ordered */
	struct log *log = &ci->home->doc_data;
	struct logbuf *b;
	int found = 0;

	if (!ci->mark || !ci->mark2)
		return Enoarg;

	if (ci->mark->ref.b == ci->mark2->ref.b) {
		if (ci->mark->ref.o < ci->mark2->ref.o)
			return 1;
		LOG("log_val_marks: same buf, bad offset: %d, %d",
		    ci->mark->ref.o, ci->mark2->ref.o);
		return Efalse;
	}
	if (ci->mark->ref.b == NULL) {
		LOG("log_val_marks: mark.b is NULL");
		return Efalse;
	}
	found = 0;
	list_for_each_entry(b, &log->log, h) {
		if (ci->mark->ref.b == b)
			found = 1;
		if (ci->mark2->ref.b == b) {
			if (found == 1)
				return 1;
			LOG("log_val_marks: mark2.b found before mark1");
			return Efalse;
		}
	}
	if (ci->mark2->ref.b == NULL) {
		if (found == 1)
			return 1;
		LOG("log_val_marks: mark2.b (NULL) found before mark1");
		return Efalse;
	}
	if (found == 0)
		LOG("log_val_marks: Neither mark found in buf list");
	if (found == 1)
		LOG("log_val_marks: mark2 not found in buf list");
	return Efalse;
}

DEF_CMD(log_destroy)
{
	/* Not allowed to destroy this document
	 * So handle command here, so we don't get
	 * to the default handler
	 */
	return 1;
}

DEF_CMD(log_view)
{
	if (!log_pane)
		return Enoarg;
	/* Not sure what I want here yet */
	attr_set_str(&log_pane->attrs, "render-default", "text");
	attr_set_str(&log_pane->attrs, "doc-type", "text");
	attr_set_str(&log_pane->attrs, "view-default", "viewer");
	call("doc:set-name", log_pane, 0, NULL, "*Debug Log*");
	call("global-multicall-doc:appeared-", log_pane);
	return 1;
}

DEF_CMD(log_close)
{
	struct log *l = &ci->home->doc_data;

	while (!list_empty(&l->log)) {
		struct logbuf *b = list_first_entry(&l->log, struct logbuf, h);
		list_del(&b->h);
		free(b);
	}
	return 1;
}

static struct map *log_map;
DEF_LOOKUP_CMD(log_handle, log_map);

DEF_CMD(log_new)
{
	struct log *l;
	struct pane *p;

	if (!ci->str)
		return Enoarg;

	p = doc_register(ci->focus, &log_handle.c);
	if (!p)
		return Efail;
	l = &p->doc_data;
	INIT_LIST_HEAD(&l->log);
	attr_set_str(&p->attrs, "render-default", "text");
	attr_set_str(&p->attrs, "doc-type", "text");
	attr_set_str(&p->attrs, "render-default", "text");
	call("doc:set-name", p, 0, NULL, ci->str);
	call("global-multicall-doc:appeared-", p);
	comm_call(ci->comm2, "cb", p);
	return 1;
}

static void log_init(struct pane *ed safe)
{
	char *fname;

	log_pane = doc_register(ed, &log_handle.c);
	if (!log_pane)
		return;
	log_doc = &log_pane->doc_data;
	INIT_LIST_HEAD(&log_doc->log);

	fname = getenv("EDLIB_LOG");
	if (!fname || !*fname)
		return;
	if (strcmp(fname, "stderr") == 0) {
		log_file = stderr;
		return;
	}

	log_file = fopen(fname, "a");
	if (!log_file)
		LOG("log: Cannot open \"%s\" for logging\n", fname);
}

void log_setup(struct pane *ed safe)
{
	log_map = key_alloc();

	key_add_chain(log_map, doc_default_cmd);
	key_add(log_map, "doc:content", &log_content);
	key_add(log_map, "doc:content-bytes", &log_content);
	key_add(log_map, "doc:set-ref", &log_set_ref);
	key_add(log_map, "doc:char", &log_char);
	key_add(log_map, "doc:destroy", &log_destroy);
	key_add(log_map, "doc:log:append", &log_append);
	key_add(log_map, "Close", &log_close);
	if(0)key_add(log_map, "debug:validate-marks", &log_val_marks);

	log_init(ed);
	call_comm("global-set-command", ed, &log_view, 0, NULL,
		  "interactive-cmd-view-log");
	call_comm("global-set-command", ed, &log_new, 0, NULL,
		  "log:create");
	LOG("log: testing 1 %d 3 Α Β Ψ α β γ", 2);
}
