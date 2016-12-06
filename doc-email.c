/*
 * Copyright Neil Brown ©2016 <neil@brown.name>
 * May be distributed under terms of GPLv2 - see file:COPYING
 *
 * doc-email: Present an email message as its intended content, with
 * part recognition and decoding etc.
 *
 * Version 0.1: Use lib-crop to display just the headers, and a separate
 *              instance to display the body.
 *
 * Not so easy.  Need to be careful about redirecting doc commands.
 * A document needs:
 *  doc:set-ref
 *  doc:mark-same
 *  doc:step
 *  doc:get-attr doc:set-attr?
 * and might capture doc:revisit to hide??
 * others are doc:load-file,same-file,save-file
 *  doc:replace doc:reundo doc:get-str doc:modified
 */

#include <unistd.h>
#include <stdlib.h>
#include <fcntl.h>
#include <string.h>
#include <stdio.h>

#define PRIVATE_DOC_REF

struct doc_ref {
	struct mark *m;
	int docnum;
};

#include "core.h"

struct email_info {
	struct doc	doc;
	struct pane	*headers safe;
	struct pane	*body safe;
};

static void reset_mark(struct mark *m)
{
	/* m->ref.m might have moved.  If so, move m in the list of
	 * marks so marks in this document are still properly ordered
	 */
	struct mark *m2;

	if (!m || hlist_unhashed(&m->all))
		return;
	while ((m2 = doc_next_mark_all(m)) != NULL &&
	       (m2->ref.docnum < m->ref.docnum ||
		(m2->ref.docnum == m->ref.docnum &&
		 m2->ref.m && m->ref.m &&
		 m2->ref.m->seq < m->ref.m->seq))) {
		/* m should be after m2 */
		mark_forward_over(m, m2);
	}

	while ((m2 = doc_prev_mark_all(m)) != NULL &&
	       (m2->ref.docnum > m->ref.docnum||
		(m2->ref.docnum == m->ref.docnum &&
		 m2->ref.m && m->ref.m &&
		 m2->ref.m->seq > m->ref.m->seq))) {
		/* m should be before m2 */
		mark_backward_over(m, m2);
	}
}

static void email_mark_refcnt(struct mark *m safe, int inc)
{
	if (inc > 0) {
		/* Duplicate being created of this mark */
		if (m->ref.m) {
			m->ref.m = mark_dup(m->ref.m, 1);
			reset_mark(m);
		}
	}
	if (inc < 0) {
		/* mark is being discarded, or ref over-written */
		if (m->ref.m)
			mark_free(m->ref.m);
		m->ref.m = NULL;
	}
}

static void email_check_consistent(struct email_info *ei safe)
{
//	struct mark *m;
	struct doc *d = &ei->doc;
//	int s = -1;

	doc_check_consistent(d);
#if 0
	for (m = doc_first_mark_all(d); m; m = doc_next_mark_all(m)) {
		if (!m->ref.m || m->ref.m->seq <= s) {
			for (m = doc_first_mark_all(d); m; m = doc_next_mark_all(m))
				if (m && m->ref.m)
					printf("%p %d %d\n", m, m->seq, m->ref.m->seq);

			abort();
		}
		s = m->ref.m->seq;
	}
	doc_check_consistent(d);
#endif
}

static void change_part(struct email_info *ei safe, struct mark *m safe, int part, int end)
{
	struct mark *m1;
	struct pane *p = part ? ei->body : ei->headers;
	if (m->ref.m)
		mark_free(m->ref.m);
	m1 = vmark_new(p, MARK_UNGROUPED);
	m->ref.m = m1;
	m->ref.docnum = part;
	m->refcnt = email_mark_refcnt;
	call3("doc:set-ref", p, !end, m1);
}

DEF_CMD(email_handle)
{
	struct email_info *ei = ci->home->data;
	struct mark *m1 = NULL, *m2 = NULL;
	int ret;

	if (strcmp(ci->key, "Close") == 0) {
		doc_free(&ei->doc);
		free(ei);
		return 1;
	}

	if (strcmp(ci->key, "doc:set-ref") != 0 &&
	    strcmp(ci->key, "doc:mark-same") != 0 &&
	    strcmp(ci->key, "doc:step") != 0 &&
	    strcmp(ci->key, "doc:get-attr") != 0
		)
		return key_lookup(doc_default_cmd, ci);

	/* Document access commands are handled by the 'cropper'.
	 * First we need to substitute the marks, then call the cropper
	 * which calls the document.  Then make sure the marks are still in order.
	 */
	if (strcmp(ci->key, "doc:set-ref") != 0)
		email_check_consistent(ei);
	if (ci->mark) {
		if (!ci->mark->ref.m) {
			change_part(ei, ci->mark, 0, 0);
			mark_to_end(&ei->doc, ci->mark, 0);
			reset_mark(ci->mark);
		}
		m1 = ci->mark->ref.m;
	}
	if (ci->mark2) {
		if (!ci->mark2->ref.m) {
			change_part(ei, ci->mark2, 0, 0);
			mark_to_end(&ei->doc, ci->mark2, 0);
			reset_mark(ci->mark2);
		}
		m2 = ci->mark2->ref.m;
	}
	if (strcmp(ci->key, "doc:set-ref") != 0)
		email_check_consistent(ei);
	if (strcmp(ci->key, "doc:mark-same") == 0 &&
	    ci->mark && ci->mark2 &&
	    ci->mark->ref.docnum != ci->mark2->ref.docnum) {
		if (ci->mark->ref.docnum < ci->mark2->ref.docnum) {
			if (call5("doc:step", ei->headers, 1, m1, NULL, 0) == CHAR_RET(WEOF) &&
			    call5("doc:step", ei->body, 0, m2, NULL, 0) == CHAR_RET(WEOF))
				return 1;
		} else if (ci->mark->ref.docnum > ci->mark2->ref.docnum) {
			if (call5("doc:step", ei->body, 0, m1, NULL, 0) == CHAR_RET(WEOF) &&
			    call5("doc:step", ei->headers, 1, m2, NULL, 0) == CHAR_RET(WEOF))
				return 1;
		}
		return 2;
	}
	if (ci->mark && ci->mark2 &&
	    ci->mark->ref.docnum != ci->mark2->ref.docnum)
		return -1;
	if (!ci->mark)
		return -1;
	if (strcmp(ci->key, "doc:set-ref") == 0 && ci->mark) {
		if (ci->numeric == 1) {
			/* start */
			if (ci->mark->ref.docnum != 0)
				change_part(ei, ci->mark, 0, 0);
		} else {
			if (ci->mark->ref.docnum != 1)
				change_part(ei, ci->mark, 1, 1);
		}
	}
	ret = call_home7(ci->mark->ref.docnum ? ei->body : ei->headers,
			 ci->key, ci->focus, ci->numeric, m1, ci->str,
			 ci->extra,ci->str2, m2, ci->comm2);
	while ((ret == CHAR_RET(WEOF) || ret == -1) &&
	       ci->mark && strcmp(ci->key, "doc:step") == 0) {
		if (ci->numeric) {
			if (ci->mark->ref.docnum == 1)
				break;
			change_part(ei, ci->mark, 1, 0);
		} else {
			if (ci->mark->ref.docnum == 0)
				break;
			change_part(ei, ci->mark, 0, 1);
		}
		m1 = ci->mark->ref.m;
		ret = call_home7(ci->mark->ref.docnum ? ei->body : ei->headers,
				 ci->key, ci->focus, ci->numeric, m1, ci->str,
				 ci->extra,ci->str2, m2, ci->comm2);
	}
	reset_mark(ci->mark);
	if (ci->mark2) {
		reset_mark(ci->mark2);
		reset_mark(ci->mark);
	}
	email_check_consistent(ei);
	return ret;
}

DEF_CMD(open_email)
{
	int fd;
	struct email_info *ei;
	struct mark *start, *end;
	wint_t prev = 0, ch = 0;
	struct pane *p, *h, *h2;

	if (ci->str == NULL ||
	    strncmp(ci->str, "email:", 6) != 0)
		return 0;
	fd = open(ci->str+6, O_RDONLY);
	p = call_pane7("doc:open", ci->focus, fd, NULL, 0, ci->str + 6, NULL);
	if (!p)
		return 0;
	start = vmark_new(p, MARK_UNGROUPED);
	if (!start)
		return 0;
	end = mark_dup(start, 1);
	while ((ch = mark_next_pane(p, end)) != WEOF) {
		if (ch == '\n' && prev == '\n')
			break;
		if (ch != '\r')
			prev = ch;
	}
	ei = calloc(1, sizeof(*ei));
	doc_init(&ei->doc);
	h = call_pane8("attach-crop", p, 0, start, end, 0, NULL, NULL);
	if (!h)
		goto out;
	h2 = call_pane("attach-rfc822header", h, 0, NULL, 0);
	if (!h2)
		goto out;
	ei->headers = h2;

	/* move 'start' to end of file */
	call3("Move-File", p, 1, start);
	h = call_pane8("attach-crop", p, 0, end, start, 0, NULL, NULL);
	if (!h)
		goto out;
	ei->body = h;

	h = pane_register(ci->home, 0, &email_handle, &ei->doc, NULL);
	if (h) {
		mark_free(start);
		mark_free(end);
		attr_set_str(&h->attrs, "render-default", "text");
		ei->doc.home = h;
		return comm_call(ci->comm2, "callback:doc", h, 0, NULL, NULL, 0);
	}
out:
	mark_free(start);
	mark_free(end);
	free(ei);
	// FIXME free stuff
	return -1;
}

void edlib_init(struct pane *ed safe)
{
	call_comm("global-set-command", ed, 0, NULL, "open-doc-email", 0, &open_email);
}
