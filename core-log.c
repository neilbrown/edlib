/*
 * Copyright Neil Brown ©2020 <neil@brown.name>
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

DEF_CMD(log_get_str)
{
	struct doc *d = ci->home->data;
	struct log *log = container_of(d, struct log, doc);
	struct mark *from = NULL, *to = NULL;
	struct logbuf *b, *first, *last;
	int l = 0, head, tail;
	char *ret;

	if (ci->mark && ci->mark2) {
		if (ci->mark2->seq < ci->mark->seq) {
			from = ci->mark2;
			to = ci->mark;
		} else {
			from = ci->mark;
			to = ci->mark2;
		}
	}

	first = list_first_entry_or_null(&log->log, struct logbuf, h);
	head = 0;
	if (from) {
		first = from->ref.b;
		if (first)
			head = from->ref.o;
	}
	last = NULL;
	tail = 0;
	if (to && to->ref.b) {
		last = to->ref.b;
		tail = to->ref.o;
	}

	b = first;
	list_for_each_entry_from(b, &log->log, h) {
		if (b == last)
			break;
		l += b->end;
	}
	l += tail - head;

	ret = malloc(l+1);

	l = 0;
	b = first;
	list_for_each_entry_from(b, &log->log, h) {
		if (b == last)
			break;
		memcpy(ret+l, b->text + head, b->end - head);
		l += b->end - head;
		head = 0;
	}
	if (last) {
		memcpy(ret+l, last->text + head, tail-head);
		l += tail - head;
	}
	ret[l] = 0;
	comm_call(ci->comm2, "callback:get-str", ci->focus, 0, NULL, ret);
	free(ret);
	return 1;
}

DEF_CMD(log_set_ref)
{
	struct doc *d = ci->home->data;
	struct log *log = container_of(d, struct log, doc);
	struct mark *m = ci->mark;

	if (!m)
		return Enoarg;
	m->ref.o = 0;
	if (ci->num == 1)
		m->ref.b = list_first_entry_or_null(&log->log, struct logbuf, h);
	else
		m->ref.b = NULL;
	mark_to_end(d, m, ci->num != 1);
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
		mark_step(m, forward);
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
	attr_set_str(&log_pane->attrs, "render-default", "text");
	call("doc:set-name", log_pane, 0, NULL, "*Debug Log*");
	call("global-multicall-doc:appeared-", log_pane);
	return 1;
}

static struct map *log_map;
DEF_LOOKUP_CMD(log_handle, log_map);

static inline void log_init(struct pane *ed safe)
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
	key_add(log_map, "doc:get-str", &log_get_str);
	key_add(log_map, "doc:set-ref", &log_set_ref);
	key_add(log_map, "doc:step", &log_step);
	key_add(log_map, "doc:destroy", &log_destroy);

	log_init(ed);
	call_comm("global-set-command", ed, &log_view, 0, NULL,
		  "interactive-cmd-view-log");
	LOG("log: testing 1 %d 3", 2);
}