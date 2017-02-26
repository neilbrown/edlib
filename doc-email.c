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
#include "core.h"

struct email_info {
	struct pane	*email safe;
};


DEF_CMD(email_close)
{
	struct email_info *ei = ci->home->data;
	// ??? ;
	free(ei);
	return 1;
}


static struct map *email_map safe;

static void email_init_map(void)
{
	email_map = key_alloc();
	key_add(email_map, "Close", &email_close);
}
DEF_LOOKUP_CMD(email_handle, email_map);

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
	p = call_pane7("doc:open", ci->focus, fd, NULL, 1, ci->str + 6, NULL);
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
	ei->email = p;
	h = call_pane8("attach-crop", p, 0, start, end, 0, NULL, NULL);
	if (!h)
		goto out;
	h2 = call_pane("attach-rfc822header", h, 0, NULL, 0);
	if (!h2)
		goto out;

	/* move 'start' to end of file */
	call3("doc:set-ref", p, 0, start);
	h = call_pane8("attach-crop", p, 0, end, start, 0, NULL, NULL);
	if (!h)
		goto out;

	p = doc_new(ci->home, "multipart", ei->email);
	if (!p)
		goto out;
	call_home(p, "multipart-add", h2, 0, NULL, NULL);
	call_home(p, "multipart-add", h, 0, NULL, NULL);
	call3("doc:autoclose", p, 1, NULL);

	h = pane_register(p, 0, &email_handle.c, ei, NULL);
	if (h) {
		mark_free(start);
		mark_free(end);
		attr_set_str(&h->attrs, "render-default", "text");
		attr_set_str(&p->attrs, "filename", ci->str+6);
		attr_set_str(&p->attrs, "doc-type", "email");
		return comm_call(ci->comm2, "callback:attach", h, 0, NULL, NULL, 0);
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
	email_init_map();
	call_comm("global-set-command", ed, 0, NULL, "open-doc-email", 0, &open_email);
}
