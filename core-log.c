/*
 * Copyright Neil Brown ©2020-2021 <neil@brown.name>
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

#define PRIVATE_DOC_REF
struct doc_ref {
	struct logbuf *b;
	unsigned int o;
};

#include "core.h"

struct logbuf {
	struct list_head h;
	unsigned int	end;
	char		text[];
};

static struct log {
	struct doc		doc;
	struct list_head	log;
} *log_doc safe;

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

	if (!(void*)log_doc)
		/* too early */
		return;
	if (!fmt)
		return;

	b = get_buf(log_doc);
	va_start(ap, fmt);
	n = vsnprintf(b->text + b->end, LBSIZE - b->end - 1, fmt, ap);
	va_end(ap);

	if (b->end != 0 && n >= LBSIZE - b->end - 1) {
		/* Didn't fit, allocate new buf */
		b = get_new_buf(log_doc);
		va_start(ap, fmt);
		n = vsnprintf(b->text, LBSIZE - 1, fmt, ap);
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
}

DEF_CMD(log_append)
{
	struct doc *d = ci->home->data;
	struct log *l = container_of(d, struct log, doc);
	struct logbuf *b;
	unsigned int len;

	if (!ci->str)
		return Enoarg;
	len = strlen(ci->str);

	b = get_buf(l);

	if (b->end != 0 && len >= LBSIZE - b->end - 1) {
		/* Doesn't fit, allocate new buf */
		b = get_new_buf(log_doc);
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
	struct doc *d = ci->home->data;
	struct log *log = container_of(d, struct log, doc);
	struct mark *from = ci->mark, *to = ci->mark2;
	struct mark *m;
	struct logbuf *b, *first, *last;
	int head, tail;
	int size = 0;

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

			if (ci->num2)
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
	struct doc *d = ci->home->data;
	struct log *log = container_of(d, struct log, doc);
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

DEF_CMD(log_step)
{
	struct doc *d = ci->home->data;
	struct log *log = container_of(d, struct log, doc);
	struct mark *m = ci->mark;
	bool forward = ci->num;
	bool move = ci->num2;
	struct doc_ref ref;
	wint_t ret;

	if (!m)
		return Enoarg;

	ref = m->ref;
	if (forward) {
		if (!ref.b)
			ret = WEOF;
		else {
			ret = ref.b->text[ref.o];
			ref.o += 1;
			if (ref.o >= ref.b->end) {
				if (ref.b == list_last_entry(&log->log,
							     struct logbuf, h))
					ref.b = NULL;
				else
					ref.b = list_next_entry(ref.b, h);
				ref.o = 0;
			}
		}
	} else {
		if (list_empty(&log->log))
			ret = WEOF;
		else if (!ref.b) {
			ref.b = list_last_entry(&log->log, struct logbuf, h);
			ref.o = ref.b->end - 1;
		} else if (ref.o == 0) {
			if (ref.b != list_first_entry(&log->log,
						      struct logbuf, h)) {
				ref.b = list_prev_entry(ref.b, h);
				ref.o = ref.b->end - 1;
			} else
				ret = WEOF;
		} else
			ref.o -= 1;
		if ((ref.b != m->ref.b || ref.o != m->ref.o) && ref.b)
			ret = ref.b->text[ref.o];
	}
	if (move) {
		mark_step(m, forward);
		m->ref = ref;
	}
	return CHAR_RET(ret);
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

static struct map *log_map;
DEF_LOOKUP_CMD(log_handle, log_map);

DEF_CMD(log_new)
{
	struct log *l;
	struct pane *p;

	if (!ci->str)
		return Enoarg;

	alloc(l, pane);
	INIT_LIST_HEAD(&l->log);
	p = doc_register(ci->focus, &log_handle.c, l);
	if (!p)
		return Efail;
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

	alloc(log_doc, pane);
	INIT_LIST_HEAD(&log_doc->log);
	log_pane = doc_register(ed, &log_handle.c, log_doc);

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
	key_add(log_map, "doc:set-ref", &log_set_ref);
	key_add(log_map, "doc:step", &log_step);
	key_add(log_map, "doc:destroy", &log_destroy);
	key_add(log_map, "doc:log:append", &log_append);

	log_init(ed);
	call_comm("global-set-command", ed, &log_view, 0, NULL,
		  "interactive-cmd-view-log");
	call_comm("global-set-command", ed, &log_new, 0, NULL,
		  "log:create");
	LOG("log: testing 1 %d 3", 2);
}
