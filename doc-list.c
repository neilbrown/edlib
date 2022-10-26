/*
 * Copyright Neil Brown ©2022 <neil@brown.name>
 * May be distributed under terms of GPLv2 - see file:COPYING
 *
 * doc-list: present an arbitrary list of items as a document.
 * This was initially created to support menus.  A doc-list is somewhat
 * similar to doc-dir or doc-docs in each each element in the document
 * has the primary content in the attributes associated with the element.
 *
 * Elements can be added after a mark with doc:list-add.  The mark remains
 * before the element so doc:set-attr can add appropriate attributes.
 *
 */

#define PRIVATE_DOC_REF

struct doc_ref {
	struct elmnt *p;
	unsigned int i;
};

#include "core.h"
#include "misc.h"

struct elmnt {
	struct list_head	list;
	struct attrset		*attrs;
};

struct list {
	struct doc		doc;
	struct list_head	content;
};

DEF_CMD(list_char)
{
	struct doc *d = ci->home->data;
	struct list *l = container_of(d, struct list, doc);
	struct mark *m = ci->mark;
	struct mark *end = ci->mark2;
	int steps = ci->num;
	int forward = steps > 0;
	int ret = Einval;

	if (!m)
		return Enoarg;

	if (end && mark_same(m, end))
		return 1;
	if (end && (end->seq < m->seq) != (steps < 0))
		/* Can never cross 'end' */
		return Einval;
	while (steps && ret != CHAR_RET(WEOF) &&
	       (!end || !mark_same(m, end))) {
		if (forward) {
			if (m->ref.p == NULL)
				ret = CHAR_RET(WEOF);
			else {
				ret = CHAR_RET(' ');
				mark_step_sharesref(m, 1);
				if (m->ref.p == list_last_entry(&l->content, struct elmnt, list))
					m->ref.p = NULL;
				else
					m->ref.p = list_next_entry(m->ref.p, list);
				steps -= 1;
			}
		} else {
			if (m->ref.p == list_first_entry_or_null(&l->content, struct elmnt, list))
				ret = CHAR_RET(WEOF);
			else {
				ret = CHAR_RET(' ');
				mark_step_sharesref(m, 0);
				if (m->ref.p == NULL)
					m->ref.p = list_last_entry(&l->content, struct elmnt, list);
				else
					m->ref.p = list_prev_entry(m->ref.p, list);
				steps += 1;
			}
		}
	}
	if (end)
		return 1 + (forward ? ci->num - steps : steps - ci->num);
	if (ret == CHAR_RET(WEOF) || ci->num2 == 0)
		return ret;
	if (ci->num && (ci->num2 < 0) == forward)
		return ret;
	/* Want the 'next' char */
	if (ci->num2 > 0 && m->ref.p == NULL)
		return CHAR_RET(WEOF);
	if (ci->num2 < 0 && m->ref.p ==
	    list_first_entry_or_null(&l->content, struct elmnt, list))
		return CHAR_RET(WEOF);
	return CHAR_RET(' ');
}

DEF_CMD(list_set_ref)
{
	struct doc *d = ci->home->data;
	struct list *l = container_of(d, struct list, doc);
	struct mark *m = ci->mark;

	if (!m)
		return Enoarg;
	mark_to_end(ci->home, m, ci->num != 1);
	if (list_empty(&l->content) || ci->num != 1)
		m->ref.p = NULL;
	else
		m->ref.p = list_first_entry(&l->content, struct elmnt, list);
	m->ref.i = 0;
	return 1;
}

DEF_CMD(list_set_attr)
{
	struct mark *m = ci->mark;
	const char *attr = ci->str;
	const char *val = ci->str2;
	struct elmnt *e;

	if (!m || !attr)
		return Enoarg;
	e = m->ref.p;
	if (!e)
		return Efallthrough;
	attr_set_str(&e->attrs, attr, val);
	pane_notify("doc:replaced-attr", ci->home, 1, m);
	return 1;
}

DEF_CMD(list_get_attr)
{
	struct mark *m = ci->mark;
	const char *attr = ci->str;
	const char *val = NULL;
	struct elmnt *e;

	if (!m || !attr)
		return Enoarg;
	e = m->ref.p;
	if (e)
		val = attr_find(e->attrs, attr);
	if (!val)
		return Efallthrough;
	comm_call(ci->comm2, "callback:get_attr", ci->focus, 0, m, val,
		  0, NULL, attr);
	return 1;
}

DEF_CMD(list_shares_ref)
{
	return 1;
}

DEF_CMD(list_add_elmnt)
{
	struct doc *d = ci->home->data;
	struct list *l = container_of(d, struct list, doc);
	struct mark *m = ci->mark;
	struct elmnt *e;

	if (!m)
		return Enoarg;
	alloc(e, pane);
	if (m->ref.p)
		list_add(&e->list, &m->ref.p->list);
	else
		list_add_tail(&e->list, &l->content);
	m->ref.p = e;
	return 1;
}

static struct map *list_map;
DEF_LOOKUP_CMD(list_handle, list_map);

DEF_CMD(list_new)
{
	struct list *l;
	struct pane *p;

	alloc(l, pane);
	INIT_LIST_HEAD(&l->content);
	p = doc_register(ci->home, &list_handle.c, l);
	if (p)
		return comm_call(ci->comm2, "callback:doc", p);
	return Efail;
}

static void list_init_map(void)
{
	list_map = key_alloc();
	key_add_chain(list_map, doc_default_cmd);
	key_add(list_map, "Free", &edlib_do_free);
	key_add(list_map, "doc:char", &list_char);
	key_add(list_map, "doc:set-ref", &list_set_ref);
	key_add(list_map, "doc:set-attr", &list_set_attr);
	key_add(list_map, "doc:get-attr", &list_get_attr);
	key_add(list_map, "doc:shares-ref", &list_shares_ref);
	key_add(list_map, "doc:list-add", &list_add_elmnt);
}

void edlib_init(struct pane *ed safe)
{
	list_init_map();
	call_comm("global-set-command", ed, &list_new, 0, NULL,
		  "attach-doc-list");
}
