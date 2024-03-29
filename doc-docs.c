/*
 * Copyright Neil Brown ©2016-2017-2019 <neil@brown.name>
 * May be distributed under terms of GPLv2 - see file:COPYING
 *
 * Document management is eased by having a well defined collection
 * of documents.  This module provides a pane and a document to manage
 * that collection.
 *
 * The document presents as a list of documents, called "*Documents*",
 * providing a "line-format" to guide display of each line.
 * The auxiliary pane becomes the parent of all attached documents, so
 * that the list of children is exactly the content of the document.
 * This pane receives doc:revisit notification  from the
 * individual documents, and also requests notification of
 * doc:status-changed.
 *
 * Supported global operations include:
 * docs:byname - report pane with given (str)name
 * docs:byfd - find a document given a path and file-descriptor.
 * Each document is asked whether it matches the path and/or fd.
 * docs:choose - choose and return a document which is not currently displayed
 * somewhere.
 * docs:save-all - each each document to save itself
 * docs:show-modified - display a pane, in given window, listing just the
 * documents that are modified and might need saving.
 * Pane auto-closes when empty.
 *
 * After a document is created and bound to a pane "doc:appeared-*" is called
 * which adds that pane to the list if it isn't already attached somewhere else.
 * If docs sees two documents with the same name, it changes one to keep them
 * all unique.
 *
 */

#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "safe.h"
#define PRIVATE_DOC_REF
struct doc_ref {
	struct pane	*p;
	unsigned int	ignore;
};
#define DOC_SHARESREF
#define DOC_DATA_TYPE struct docs
#define DOC_NEXT(d,m,r,b) docs_next(d,r,b)
#define DOC_PREV(d,m,r,b) docs_prev(d,r,b)

#define PANE_DATA_PTR_TYPE struct pane *
#define PANE_DATA_VOID_2
#include "core.h"

static struct map *docs_map, *docs_aux_map, *docs_modified_map,
	*docs_callback_map;
DEF_LOOKUP_CMD(docs_handle, docs_map);
DEF_LOOKUP_CMD(docs_aux, docs_aux_map);
DEF_LOOKUP_CMD(docs_modified_handle, docs_modified_map);
DEF_LOOKUP_CMD(docs_callback_handle, docs_callback_map);

struct docs {
	struct doc		doc;
	struct command		callback;
	struct pane		*collection safe;
};
#include "core-pane.h"

static void docs_demark(struct pane *d safe, struct pane *p safe)
{
	/* This document (p) is about to be moved in the list (d->collection).
	 * Any mark pointing at it is moved forward
	 */
	struct docs *doc = d->doc_data;
	struct mark *m, *first = NULL;
	struct pane *next;
	struct pane *col = doc->collection;

	if (list_empty(&p->siblings) ||
	    p == list_last_entry(&col->children, struct pane, siblings))
		next = NULL;
	else
		next = list_next_entry(p, siblings);

	for (m = mark_first(&doc->doc);
	     m;
	     m = mark_next(m))
		if (m->ref.p == p) {
			if (!first) {
				first = mark_prev(m);
				if (!first)
					first = m;
			}
			m->ref.p = next;
		} else if (first)
			break;
	if (first)
		pane_notify("doc:replaced", d, 1, first);
}

static void docs_enmark(struct pane *d safe, struct pane *p safe)
{
	/* This document has just been added to the list.
	 * any mark pointing just past it is moved back.
	 */
	struct docs *doc = d->doc_data;
	struct mark *m, *first = NULL;
	struct pane *next;
	struct pane *col = doc->collection;

	if (p == list_last_entry(&col->children, struct pane, siblings))
		next = NULL;
	else
		next = list_next_entry(p, siblings);

	for (m = mark_first(&doc->doc);
	     m;
	     m = mark_next(m))
		if (m->ref.p == next) {
			if (!first)
				first = m;
			m->ref.p = p;
		} else if (first)
			break;
	if (first)
		pane_notify("doc:replaced", d, 1, first);
}

static bool doc_save(struct pane *p safe, struct pane *focus safe, int test)
{
	char *fn = pane_attr_get(p, "filename");
	char *mod = pane_attr_get(p, "doc-modified");
	if (!fn || !*fn)
		call("Message", focus, 0, NULL,
		     "File has no filename - cannot be saved.");
	else if (!mod || strcmp(mod, "yes") != 0)
		call("Message", focus, 0, NULL,
		     "File not modified - no need to save.");
	else if (test)
		return True;
	else
		home_call(p, "doc:save-file", focus);
	return False;
}

static void check_name(struct docs *docs safe, struct pane *pane safe)
{
	struct doc *d = &pane->doc;
	char *nname;
	int unique = 1;
	int conflict = 1;

	if (!d->name)
		d->name = strdup("*unknown*");

	nname = malloc(strlen(d->name) + sizeof("<xxx>"));
	while (conflict && unique < 1000) {
		struct pane *p;
		conflict = 0;
		if (unique > 1)
			sprintf(nname, "%s<%d>", d->name, unique);
		else
			strcpy(nname, d->name);
		list_for_each_entry(p, &docs->collection->children, siblings) {
			struct doc *d2 = &p->doc;
			if (d != d2 && d2->name &&
			    strcmp(nname, d2->name) == 0) {
				conflict = 1;
				unique += 1;
				break;
			}
		}
	}
	if (unique > 1) {
		free(d->name);
		d->name = nname;
	} else
		free(nname);
}

static void doc_checkname(struct pane *p safe, struct pane *d safe, int n)
{
	struct docs *ds = d->doc_data;
	ASSERT(p->parent->handle == &docs_aux.c);
	check_name(ds, p);
	if (n) {
		docs_demark(d, p);
		if (n > 0)
			list_move(&p->siblings, &ds->collection->children);
		else
			list_move_tail(&p->siblings, &ds->collection->children);
		docs_enmark(d, p);
	}
}

/*
 * Interactive saving of files, particularly as happens when the editor
 * is exiting, pops up a document-list window which only display
 * documents which need saving.  They can be saved or killed, both of which
 * actions removes them from the list.  When the list is empty an event can be
 * sent back to the pane that requested the popup.
 */

static int docs_open(struct pane *home safe, struct pane *focus safe,
		     struct mark *m, bool other);

DEF_CMD(docs_mod_next)
{
	struct mark *m;

	/* If this is the last, then quit */
	if (!ci->mark)
		return Enoarg;
	m = mark_dup(ci->mark);
	call("doc:EOL", ci->home->parent, 1, m, NULL, 1);
	/* Passing '0' is deliberate.  We don't want to render
	 * anything, just see if there is anything tha could be rendered.
	 */
	if (call("doc:render-line", ci->focus, 0, m) < 0 ||
	    m->ref.p == NULL) {
		mark_free(m);
		return call("popup:close", ci->focus);
	}
	mark_free(m);
	/* Ask viewer to move forward */
	return 2;
}

DEF_CMD(docs_mod_quit)
{
	return call("popup:close", ci->home);
}

DEF_CMD(docs_mod_other)
{
	/* abort the current action, and open this in another window */
	docs_open(ci->home, ci->focus, ci->mark, True);
	call("Abort", ci->home);
	return 1;
}

DEF_CMD(docs_mod_empty)
{
	call("popup:close", ci->focus);
	return 1;
}

DEF_CMD(docs_mod_noop)
{
	/* Don't want anything else to fall through to default */
	return 1;
}

DEF_CMD(docs_callback_complete)
{
	struct pane *p;

	p = home_call_ret(pane, ci->home, "doc:attach-view", ci->focus,
			  0, NULL, "simple");
	if (p) {
		attr_set_str(&p->attrs, "line-format", "%doc-name");
		attr_set_str(&p->attrs, "heading", "");
		attr_set_str(&p->attrs, "done-key", "Replace");
		p = call_ret(pane, "attach-render-complete", p);
	}
	if (p)
		return comm_call(ci->comm2, "callback:doc", p);
	return Efail;
}

DEF_CMD(docs_callback_byname)
{
	struct docs *doc = ci->home->doc_data;
	struct pane *p;

	if (ci->str == NULL || strcmp(ci->str, "*Documents*") == 0)
		return comm_call(ci->comm2, "callback:doc",
				 ci->home);
	list_for_each_entry(p, &doc->collection->children, siblings) {
		struct doc *dc = &p->doc;
		char *n = dc->name;
		if (n && strcmp(ci->str, n) == 0)
			return comm_call(ci->comm2, "callback:doc", p);
	}
	return Efail;
}

DEF_CMD(docs_callback_byfd)
{
	struct docs *doc = ci->home->doc_data;
	struct pane *p;

	list_for_each_entry(p, &doc->collection->children, siblings) {
		if (call("doc:same-file", p, 0, NULL, ci->str,
			 ci->num2) > 0)
			return comm_call(ci->comm2, "callback:doc", p);
	}
	return Efail;
}

DEF_CMD(docs_callback_byeach)
{
	struct docs *doc = ci->home->doc_data;
	struct pane *p;
	int ret = 1;

	list_for_each_entry(p, &doc->collection->children, siblings) {
		int r;
		r = comm_call(ci->comm2, "callback:doc", p);
		if (r > ret)
			ret = r;
		if (r == Efalse)
			return ret;
		if (r < Efalse)
			return r;
	}
	return ret;
}

DEF_CMD(docs_callback_choose)
{
	struct docs *doc = ci->home->doc_data;
	struct pane *choice = NULL, *last = NULL;
	struct pane *p;

	/* Choose a document with no notifiees or no pointer,
	 * but ignore 'CLOSED'
	 */

	list_for_each_entry(p, &doc->collection->children, siblings) {
		struct doc *d = &p->doc;

		if (p->damaged & DAMAGED_CLOSED)
			continue;
		last = p;
		if (list_empty(&p->notifiees)) {
			choice = p;
			break;
		}
		if (tlist_empty(&d->points)) {
			choice = p;
			break;
		}
	}
	if (!choice)
		choice = last;
	if (!choice)
		choice = ci->home;
	return comm_call(ci->comm2, "callback:doc", choice);
}

DEF_CMD(docs_callback_saveall)
{
	struct docs *doc = ci->home->doc_data;
	struct pane *p;
	int dirlen = ci->str ? (int)strlen(ci->str) : -1;

	list_for_each_entry(p, &doc->collection->children, siblings) {
		if (dirlen > 0) {
			char *fn = pane_attr_get(p, "dirname");
			if (!fn || strncmp(ci->str, fn, dirlen) != 0)
				continue;
		}
		if (doc_save(p, p, ci->num2))
			/* Something needs to be saved, we were only asked
			 * to test.
			 */
			return 2;
	}
	return 1;
}

DEF_CMD(docs_callback_modified)
{
	struct pane *p;

	p = home_call_ret(pane, ci->home, "doc:attach-view", ci->focus,
			  0, NULL, "simple");
	if (!p)
		return Efail;
	p = call_ret(pane, "attach-linefilter", p);
	if (!p)
		return Efail;
	attr_set_str(&p->attrs, "filter:attr", "doc-can-save");
	attr_set_str(&p->attrs, "filter:match", "yes");
	p = pane_register_2(p, 0, &docs_modified_handle.c);
	if (!p)
		return Efail;
	attr_set_str(&p->attrs, "doc-name", "*Modified Documents*");
	attr_set_str(&p->attrs, "line-format", "%doc-name:20 %filename");
	attr_set_str(&p->attrs, "heading",
		     "<bold>Document             File</>\n"
		     "<bold,underline>[s]ave [y]es [n]o [q]uit</>");
	/* Don't want to inherit position from some earlier instance,
	 * always move to the start.
	 */
	call("doc:file", p, -1);
	return 1;
}

DEF_CMD(docs_callback_appeared)
{
	struct docs *doc = ci->home->doc_data;
	struct pane *p;

	/* Always return Efallthrough so other handlers get a chance */
	p = ci->focus;
	if (!p)
		return Efallthrough;
	if (p->parent != p->parent->parent)
		/* This has a parent which is not the root,
		 * so we shouldn't interfere.
		 */
		return Efallthrough;
	if (p == ci->home)
		/* The docs doc is attached separately */
		return Efallthrough;
	pane_reparent(p, doc->collection);
	home_call(p, "doc:request:doc:revisit", doc->collection);
	home_call(p, "doc:request:doc:status-changed",
		  doc->collection);
	doc_checkname(p, ci->home, ci->num ?: -1);

	return Efallthrough;
}

DEF_CMD(doc_damage)
{
	struct pane *dp = ci->home->data;
	struct mark *m = mark_new(dp);
	struct pane *child = ci->focus;

	if (!child || !m)
		return Enoarg;
	do {
		if (m->ref.p == child) {
			pane_notify("doc:replaced", dp, 1, m);
			break;
		}
	} while (doc_next(dp, m) != WEOF);
	mark_free(m);
	return 1;
}

DEF_CMD(doc_revisit)
{
	struct pane *p = ci->focus;
	struct pane *dp = ci->home->data;
	struct docs *docs = dp->doc_data;

	if (!p)
		return Einval;
	if (p->parent != docs->collection)
		return Efallthrough;
	if (p == ci->home)
		return 1;
	doc_checkname(p, dp, ci->num);
	return 1;
}

static inline wint_t docs_next(struct pane *home safe, struct doc_ref *r safe, bool bytes)
{
	struct docs *d = home->doc_data;
	struct pane *p = r->p;

	if (p == NULL)
		return WEOF;

	if (p == list_last_entry(&d->collection->children,
				 struct pane, siblings))
		r->p = NULL;
	else
		r->p = list_next_entry(p, siblings);
	return '\n';
}
static inline wint_t docs_prev(struct pane *home safe, struct doc_ref *r safe, bool bytes)
{
	struct docs *d = home->doc_data;
	struct pane *p = r->p;

	if (list_empty(&d->collection->children))
		return WEOF;
	else if (!p)
		p = list_last_entry(&d->collection->children,
				    struct pane, siblings);
	else if (p != list_first_entry(&d->collection->children,
				       struct pane, siblings))
		p = list_prev_entry(p, siblings);
	else
		return WEOF;
	r->p = p;
	return '\n';
}

DEF_CMD(docs_char)
{
	return do_char_byte(ci);
}

DEF_CMD(docs_set_ref)
{
	struct docs *d = ci->home->doc_data;
	struct mark *m = ci->mark;

	if (!m)
		return Enoarg;

	mark_to_end(ci->home, m, ci->num != 1);
	if (ci->num == 1 && !list_empty(&d->collection->children))
		m->ref.p = list_first_entry(&d->collection->children,
					    struct pane, siblings);
	else
		m->ref.p = NULL;

	m->ref.ignore = 0;
	return 1;
}

DEF_CMD(docs_doc_get_attr)
{
	struct mark *m = ci->mark;
	const char *attr = ci->str;
	char *val;

	if (!m || !attr)
		return Enoarg;

	if (!m->ref.p)
		return Efallthrough;

	val = pane_attr_get(m->ref.p, attr);
	/* use 'while' instead of 'if' to allow 'break' */
	while (!val && strcmp(attr, "doc-can-save") == 0) {
		char *mod, *fl, *dir;
		val = "no";
		mod = pane_attr_get(m->ref.p, "doc-modified");
		if (!mod || strcmp(mod, "yes") != 0)
			break;
		fl = pane_attr_get(m->ref.p, "filename");
		if (!fl || !*fl)
			break;
		dir = pane_attr_get(ci->focus, "only-here");
		if (!dir || strncmp(dir, fl, strlen(dir)) == 0)
			val = "yes";
	}

	if (!val)
		return Efallthrough;
	comm_call(ci->comm2, "callback:get_attr", ci->focus, 0, m, val,
		  0, NULL, attr);
	return 1;
}

DEF_CMD(docs_get_attr)
{
	const char *attr = ci->str;
	char *val;

	if (!attr)
		return Enoarg;

	if ((val = attr_find(ci->home->attrs, attr)) != NULL)
		;
	else if (strcmp(attr, "heading") == 0)
		val = "<bold,underline> Mod Document             File</>";
	else if (strcmp(attr, "line-format") == 0)
		val = " %doc-modified:3 %doc-name:20 %filename";
	else if (strcmp(attr, "render-default") == 0)
		val = "format";
	else if (strcmp(attr, "render-simple") == 0)
		val = "format";
	else if (strcmp(attr, "view-default") == 0)
		val = "viewer";
	else if (strcmp(attr, "doc-type") == 0)
		val = "docs";
	else
		return Efallthrough;

	comm_call(ci->comm2, "callback:get_attr", ci->focus,
		  0, NULL, val);
	return 1;
}

static int docs_open(struct pane *home safe, struct pane *focus safe,
		     struct mark *m, bool other)
{
	struct pane *p = NULL;
	struct pane *dp;
	struct pane *par;

	if (!m)
		return Enoarg;
	dp = m->ref.p;
	/* close this pane, open the given document. */
	if (dp == NULL)
		return Efail;

	if (other) {
		par = home_call_ret(pane, focus, "DocPane", dp);
		if (par) {
			pane_take_focus(par);
			return 1;
		}
		par = call_ret(pane, "OtherPane", focus);
	} else
		par = call_ret(pane, "ThisPane", focus);
	if (par)
		p = home_call_ret(pane, dp, "doc:attach-view", par, 1);
	if (p) {
		pane_take_focus(p);
		return 1;
	} else {
		return Efail;
	}
}

static int docs_open_alt(struct pane *home safe, struct pane *focus safe,
			 struct mark *m, char cmd)
{
	struct pane *p;
	struct pane *dp;
	char *renderer = NULL;
	char *viewer = NULL;
	struct pane *par;
	char buf[100];

	if (!m)
		return Enoarg;
	dp = m->ref.p;
	/* close this pane, open the given document. */
	if (dp == NULL)
		return Efail;

	snprintf(buf, sizeof(buf), "render-cmd-%c", cmd);
	renderer = pane_attr_get(dp, buf);
	snprintf(buf, sizeof(buf), "view-cmd-%c", cmd);
	viewer = pane_attr_get(dp, buf);
	if (!renderer && !viewer)
		return Efail;

	par = call_ret(pane, "ThisPane", focus);
	if (!par)
		return Efail;
	p = home_call_ret(pane, dp, "doc:attach-view", par, 1, NULL, buf+5);
	if (p) {
		pane_take_focus(p);
		return 1;
	} else {
		return Efail;
	}
}

static int docs_bury(struct pane *focus safe)
{
	/* If the docs list is in a tile, put something else there. */
	/* FIXME should this be a function of the pane manager? */
	struct pane *tile, *doc;
	tile = call_ret(pane, "ThisPane", focus);
	if (!tile)
		return 1;
	/* Discourage this doc from being chosen again */
	call("doc:notify:doc:revisit", focus, -1);
	doc = call_ret(pane, "docs:choose", focus);
	if (doc)
		home_call(doc, "doc:attach-view", tile);
	return 1;
}

static int docs_save(struct pane *focus safe, struct mark *m)
{
	struct pane *dp;

	if (!m)
		return Enoarg;
	dp = m->ref.p;
	if (!dp)
		return Efail;
	doc_save(dp, focus, 0);
	return 1;
}

static int docs_kill(struct pane *focus safe, struct mark *m, int num)
{
	struct pane *dp;
	char *mod;

	if (!m)
		return Enoarg;
	dp = m->ref.p;
	if (!dp)
		return Efail;
	mod = pane_attr_get(dp, "doc-modified");
	if (mod && strcmp(mod, "yes") == 0 &&
	    num == NO_NUMERIC) {
		call("Message", focus, 0, NULL,
		     "File modified, cannot kill.");
		return 1;
	}
	call("doc:destroy", dp);
	return 1;
}

DEF_CMD(docs_destroy)
{
	/* Not allowed to destroy this document
	 * So handle command here, so we don't get
	 * to the default handler
	 */
	return 1;
}

DEF_CMD(docs_child_closed)
{
	struct pane *pd = ci->home->data;

	if (ci->num < 0)
	    docs_demark(pd, ci->focus);
	return 1;
}

DEF_CMD(docs_do_open)
{
	return docs_open(ci->home, ci->focus, ci->mark, False);
}

DEF_CMD(docs_do_open_other)
{
	return docs_open(ci->home, ci->focus, ci->mark, True);
}

DEF_CMD(docs_do_open_alt)
{
	const char *c = ksuffix(ci, "doc:cmd-");

	return docs_open_alt(ci->home,
			     ci->focus, ci->mark, c[0]);
}

DEF_CMD(docs_do_quit)
{
	return docs_bury(ci->focus);
}

DEF_CMD(docs_do_save)
{
	return docs_save(ci->focus, ci->mark);
}

DEF_CMD(docs_do_kill)
{
	return docs_kill(ci->focus, ci->mark, ci->num);
}

DEF_CMD(docs_shares_ref)
{
	return 1;
}

DEF_CMD(docs_val_marks)
{
	struct docs *d = ci->home->doc_data;
	struct pane *p;
	int found;

	if (!ci->mark || !ci->mark2)
		return Enoarg;

	if (ci->mark->ref.p == ci->mark2->ref.p) {
		if (ci->mark->ref.ignore < ci->mark2->ref.ignore)
			return 1;
		LOG("docs_val_marks: same buf, bad offset: %u, %u",
		    ci->mark->ref.ignore, ci->mark2->ref.ignore);
		return Efalse;
	}
	if (ci->mark->ref.p == NULL) {
		LOG("docs_val_marks: mark.p is NULL");
		return Efalse;
	}
	found = 0;
	list_for_each_entry(p, &d->collection->children, siblings) {
		if (ci->mark->ref.p == p)
			found = 1;
		if (ci->mark2->ref.p == p) {
			if (found == 1)
				return 1;
			LOG("docs_val_marks: mark2.p found before mark1");
			return Efalse;
		}
	}
	if (ci->mark2->ref.p == NULL) {
		if (found == 1)
			return 1;
		LOG("docs_val_marks: mark2.p (NULL) found before mark1");
		return Efalse;
	}
	if (found == 0)
		LOG("docs_val_marks: Neither mark found in pane list");
	if (found == 1)
		LOG("docs_val_marks: mark2 not found in pane list");
	return Efalse;
}

DEF_CMD_CLOSED(docs_close)
{
	struct docs *docs = ci->home->doc_data;

	call_comm("global-set-command-prefix", ci->home, &edlib_noop,
		  0, NULL, "docs:");
	call_comm("global-set-command", ci->home, &edlib_noop,
		  0, NULL, "doc:appeared-docs-register");
	pane_close(docs->collection);
	return Efallthrough;
}

static void docs_init_map(void)
{
	if (docs_map)
		return;
	docs_map = key_alloc();
	docs_aux_map = key_alloc();
	docs_modified_map = key_alloc();
	docs_callback_map = key_alloc();
	/* A "docs" document provides services to children and also behaves as
	 * a document which lists those children
	 */
	key_add_chain(docs_map, doc_default_cmd);
	key_add(docs_map, "doc:set-ref", &docs_set_ref);
	key_add(docs_map, "doc:get-attr", &docs_doc_get_attr);
	key_add(docs_map, "doc:char", &docs_char);
	key_add(docs_map, "doc:destroy", &docs_destroy);
	key_add(docs_map, "doc:cmd-f", &docs_do_open);
	key_add(docs_map, "doc:cmd-\n", &docs_do_open);
	key_add(docs_map, "doc:cmd:Enter", &docs_do_open);
	key_add(docs_map, "doc:cmd-o", &docs_do_open_other);
	key_add(docs_map, "doc:cmd-q", &docs_do_quit);
	key_add(docs_map, "doc:cmd-s", &docs_do_save);
	key_add(docs_map, "doc:cmd-k", &docs_do_kill);
	key_add_range(docs_map, "doc:cmd-A", "doc:cmd-Z", &docs_do_open_alt);
	key_add(docs_map, "doc:shares-ref", &docs_shares_ref);
	if(0)key_add(docs_map, "debug:validate-marks", &docs_val_marks);

	key_add(docs_map, "get-attr", &docs_get_attr);
	key_add(docs_map, "Close", &docs_close);

	key_add(docs_aux_map, "doc:revisit", &doc_revisit);
	key_add(docs_aux_map, "doc:status-changed", &doc_damage);
	key_add(docs_aux_map, "Child-Notify", &docs_child_closed);

	key_add_prefix(docs_modified_map, "doc:cmd-", &docs_mod_noop);
	key_add_prefix(docs_modified_map, "doc:cmd:", &docs_mod_noop);
	key_add(docs_modified_map, "doc:cmd-s", &docs_do_save);
	key_add(docs_modified_map, "doc:cmd-y", &docs_do_save);
	key_add(docs_modified_map, "doc:cmd-n", &docs_mod_next);
	key_add(docs_modified_map, "doc:cmd-q", &docs_mod_quit);
	key_add(docs_modified_map, "doc:cmd-o", &docs_mod_other);

	key_add(docs_modified_map, "Notify:filter:empty", &docs_mod_empty);

	key_add(docs_callback_map, "docs:complete", &docs_callback_complete);
	key_add(docs_callback_map, "docs:byname", &docs_callback_byname);
	key_add(docs_callback_map, "docs:byfd", &docs_callback_byfd);
	key_add(docs_callback_map, "docs:byeach", &docs_callback_byeach);
	key_add(docs_callback_map, "docs:choose", &docs_callback_choose);
	key_add(docs_callback_map, "docs:save-all", &docs_callback_saveall);
	key_add(docs_callback_map, "docs:show-modified",
		&docs_callback_modified);
	key_add(docs_callback_map, "doc:appeared-docs-register",
		&docs_callback_appeared);
}

DEF_CB(docs_callback_lookup)
{
	struct docs *docs = container_of(ci->comm, struct docs, callback);
	struct pane *home = docs->collection->data;

	return do_call_val(TYPE_comm, home, &docs_callback_handle.c,
			   ci->key, ci->focus,
			   ci->num, ci->mark, ci->str,
			   ci->num2, ci->mark2, ci->str2,
			   ci->x, ci->y, ci->comm2);
}

DEF_CMD(attach_docs)
{
	/* Attach a docs handler.  We register some commands with the editor
	 * so we can be found
	 */
	struct docs *doc;
	struct pane *pd, *paux;

	docs_init_map();

	pd = doc_register(ci->home, &docs_handle.c);
	if (!pd)
		return Efail;
	doc = pd->doc_data;
	doc->doc.name = strdup("*Documents*");
	paux = pane_register(ci->home, 0, &docs_aux.c, pd);
	if (!paux) {
		pane_close(pd);
		return Efail;
	}
	doc->collection = paux;

	doc->callback = docs_callback_lookup;
	call_comm("global-set-command-prefix", ci->home, &doc->callback,
		  0, NULL, "docs:");
	call_comm("global-set-command", ci->home, &doc->callback,
		  0, NULL, "doc:appeared-docs-register");

	pane_reparent(pd, doc->collection);

	return comm_call(ci->comm2, "callback:doc", pd);
}

void edlib_init(struct pane *ed safe)
{
	call_comm("global-set-command", ed, &attach_docs, 0, NULL,
		  "attach-doc-docs");
}
