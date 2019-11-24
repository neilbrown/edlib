/*
 * Copyright Neil Brown ©2019 <neil@brown.name>
 * May be distributed under terms of GPLv2 - see file:COPYING
 *
 * A virtual document which comprises characters from the
 * rendering of another document.
 * To determine the content of this document, we call "render-line"
 * on the underlying document, and treat each non-attribute
 * character in the result as a character in this document.
 * This is particularly useful for 'render-format' documents
 * as it allows a mark to point into the rendering, so that
 * substrings can be highlighted or copied.
 *
 * Every mark in this document refers to a mark in the
 * underlying document.  We call 'render-line' from that
 * mark and store the result in mark->mdata.
 */

#include <string.h>

#define PRIVATE_DOC_REF
struct doc_ref {
	struct mark *m;
	int offset;
};
#define MARK_DATA_PTR char
#include "core.h"

struct dr_info {
	struct doc	doc;
	int		vnum;
	struct pane	*base safe;
};

static struct map *dr_map safe;
DEF_LOOKUP_CMD(dr_handle, dr_map);

static void drop_ref_mark(struct mark *m)
{
	struct mark *refm;

	if (!m)
		return;
	refm = m->ref.m;
	m->ref.m = NULL;
	m->ref.offset = 0;
	if (!refm)
		return;
	if (--refm->refs > 0)
		return;
	free(refm->mdata);
	refm->mdata = NULL;
	mark_free(refm);
}

static void reposition_mark(struct mark *m safe)
{
	struct mark *m2;

	if (!m->ref.m)
		return;
	m2 = doc_next_mark_all(m);
	while (m2) {
		if (m2->ref.m == NULL)
			break;
		if (m2->ref.m->seq > m->ref.m->seq)
			break;
		if (m2->ref.m == m->ref.m &&
		    (unsigned int)m2->ref.offset > (unsigned int)m->ref.offset)
			break;
		/* m needs to be after m2 */
		mark_to_mark_noref(m, m2);
		m2 = doc_next_mark_all(m);
	}

	m2 = doc_prev_mark_all(m);
	while (m2) {
		if (m2->ref.m == NULL)
			break;
		if (m2->ref.m->seq < m->ref.m->seq)
			break;
		if (m2->ref.m == m->ref.m &&
		    (unsigned int)m2->ref.offset < (unsigned int)m->ref.offset)
			break;
		/* m needs to be before  m2 */
		mark_to_mark_noref(m, m2);
		m2 = doc_prev_mark_all(m);
	}
}


static void set_ref_mark(struct pane *home safe, struct mark *m safe,
			 struct pane *p safe, int view, struct mark *loc safe)
{
	/* Set m->ref to refer to the given location in the given pane */
	struct mark *m2;

	if (m->ref.m && mark_same(m->ref.m, loc))
		return;
	drop_ref_mark(m);
	m2 = vmark_at_or_before(p, loc, view, home);
	if (!m2 || !mark_same(m2, loc)) {
		/* No suitable mark */
		m2 = vmark_new(p, view, home);
		if (!m2)
			return;
		mark_to_mark(m2, loc);
		m2->mdata = NULL;
	}
	m2->refs += 1;
	m->ref.m = m2;
	m->ref.offset = 0;
	reposition_mark(m);
}

static void dr_refcnt(struct mark *m safe, int inc)
{
	if (!m->ref.m)
		return;
	if (inc > 0)
		m->ref.m->refs += 1;
	if (inc < 0) {
		m->ref.m->refs -= 1;
		if (m->ref.m->refs == 0) {
			free(m->ref.m->mdata);
			m->ref.m->mdata = NULL;
			mark_free(m->ref.m);
		}
		m->ref.m = NULL;
	}
}

DEF_CMD(dr_set_ref)
{
	struct doc *d = ci->home->data;
	struct dr_info *dri = container_of(d, struct dr_info, doc);
	struct pane *p = dri->base;
	struct mark *m = ci->mark;
	struct mark *m2;

	if (!m || !p)
		return Enoarg;
	drop_ref_mark(m);

	m2 = vmark_new(p, MARK_UNGROUPED, NULL);
	if (!m2)
		return Efail;
	call("doc:set-ref", p, ci->num, m2);

	m->ref.offset = 0;
	mark_to_end(d, m, ci->num != 1);

	set_ref_mark(ci->home, m, p, dri->vnum, m2);
	mark_free(m2);
	return 1;
}

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

/* The offset may be immediately  before attributes,
 * but never immediately after attributes.
 * offset may never be in middle of '<<'
 * It may be at start of line (0) but never at
 * end of line (unless at end-of-file)
 * The caller of dr_next() must check for eol
 * and of dr_prev must check for sol.
 */

static wint_t dr_next(char *line safe, int *op safe)
{
	int o = *op;
	wchar_t ret;
	mbstate_t ps = {};
	int err;

	while (line[o] == '<' && line[o+1] != '<') {
		while (line[o] && line[o] != '>')
			o += 1;
		if (line[o])
			o += 1;
	}
	err = mbrtowc(&ret, line+o, 4, &ps);
	if (err < 0) {
		ret = line[o];
		err = 1;
	}
	if (ret == '<' && line[o+1] == '<')
		err += 1;
	*op = o+err;
	return ret;
}

static wint_t dr_prev(char *line safe, int *op safe)
{
	int o = *op;
	wchar_t ret;
	mbstate_t ps = {};
	int err;

	if (o == 0)
		return WEOF;
	o = text_round_len(line, o-1);
	err = mbrtowc(&ret, line + o, *op - o, &ps);
	if (err < 0) {
		o = *op - 1;
		err = 1;
		ret = line[o];
	}
	if (o > 0 && line[o-1] == '>') {
		/* Need to search from start to find previous
		 * char.
		 */
		int oprev, otmp;
		oprev = 0; otmp = 0;
		while (otmp < o) {
			if (line[otmp] != '<') {
				oprev = otmp;
				otmp += 1;
				continue;
			}
			if (line[otmp+1] == '<') {
				oprev = otmp;
				otmp += 2;
				continue;
			}
			while (line[otmp] != '>')
				otmp += 1;
			otmp += 1;
		}
		o = oprev;
	}
	*op = o;
	return ret;
}

DEF_CMD(dr_step)
{
	struct doc *d = ci->home->data;
	struct dr_info *dri = container_of(d, struct dr_info, doc);
	struct pane *p = dri->base;
	struct mark *m = ci->mark;
	int forward = ci->num;
	int do_move = ci->num2;
	char *line;
	wint_t ret;

	if (!m || !p || !m->ref.m)
		return Enoarg;

	line = m->ref.m->mdata;
	if (!line) {
		struct mark *tmp = mark_dup(m->ref.m);
		if (tmp)
			m->ref.m->mdata =
				call_ret(str, "doc:render-line", p,
					 NO_NUMERIC, tmp);
		mark_free(tmp);
		if (!m->ref.m->mdata)
			m->ref.m->mdata = strdup("");
		line = m->ref.m->mdata;
	}

	if (!line)
		return Efail;
	if (m->ref.offset < 0)
		m->ref.offset = strlen(line) - 1;
	if (forward) {
		struct mark *loc;
		int len = strlen(line);
		int o = m->ref.offset;
		if (o >= len)
			/* Must be EOF */
			return CHAR_RET(WEOF);
		ret = dr_next(line, &o);
		if (!do_move)
			return CHAR_RET(ret);
		m->ref.offset = o;
		reposition_mark(m);
		if (o < len)
			return CHAR_RET(ret);
		/* Need to move to next line */
		loc = mark_dup(m->ref.m);
		call("doc:render-line", p, NO_NUMERIC, loc);
		set_ref_mark(ci->home, m, p, dri->vnum, loc);
		mark_free(loc);
		return CHAR_RET(ret);
	} else {
		int o = m->ref.offset;
		if (o == 0) {
			struct mark *loc = mark_dup(m->ref.m);
			if (call("doc:render-line-prev", p, 1, loc) < 0) {
				/* at start-of-doc */
				mark_free(loc);
				return CHAR_RET(WEOF);
			}
			ret = '\n';
			if (!do_move) {
				mark_free(loc);
				return CHAR_RET(ret);
			}
			set_ref_mark(ci->home, m, p, dri->vnum, loc);
			m->ref.offset = -1;
			reposition_mark(m);
			mark_free(loc);
			return CHAR_RET(ret);
		}
		ret = dr_prev(line, &o);
		if (do_move) {
			m->ref.offset = o;
			reposition_mark(m);
		}
		return CHAR_RET(ret);
	}
}


DEF_CMD(dr_close)
{
	struct doc *d = ci->home->data;
	struct dr_info *dri = container_of(d, struct dr_info, doc);
	struct pane *p = dri->base;
	struct mark *m;

	if (!p)
		return Einval;

	m = doc_first_mark_all(&dri->doc);
	while (m) {
		if (m->ref.m) {
			m->ref.m->refs --;
			m->ref.m = NULL;
		}
		m = doc_next_mark_all(m);
	}

	m = vmark_first(p, dri->vnum, ci->home);
	while (m) {
		struct mark *tmp = vmark_next(m);
		free(m->mdata);
		m->mdata = NULL;
		mark_free(m);
		m = tmp;
	}

	if (p)
		home_call(p, "doc:del-view", ci->home, dri->vnum);
	doc_free(d);
	free(dri);
	ci->home->data = safe_cast NULL;
	return 1;
}

DEF_CMD(dr_notify_viewers)
{
	/* Yes, I'm stil viewing this document */
	return 1;
}

DEF_CMD(dr_notify_replace)
{
	/* Something has changed, invalidate all cached content */
	struct doc *d = ci->home->data;
	struct dr_info *dri = container_of(d, struct dr_info, doc);
	struct pane *p = dri->base;
	struct mark *first = ci->mark;
	struct mark *last = ci->mark2;
	struct mark *start = NULL, *end = NULL;
	struct mark *m;

	if (!p)
		return Einval;
	if (first && last && first->seq > last->seq) {
		first = ci->mark2;
		last = ci->mark;
	}
	if (!first)
		first = last;
	if (!last)
		last = first;
	m = vmark_first(p, dri->vnum, ci->home);
	while (m && (!last || mark_ordered_or_same(m, last))) {
		if (!first || mark_ordered_or_same(first,m)) {
			if (!start) {
				start = vmark_new(ci->home, MARK_UNGROUPED,
						  NULL);
				if (start)
					set_ref_mark(ci->home, start, p, dri->vnum, m);
			}
			free(m->mdata);
			m->mdata = NULL;
		}
		m = vmark_next(m);
	}
	if (m) {
		end =vmark_new(ci->home, MARK_UNGROUPED,
			       NULL);
		if (end)
			set_ref_mark(ci->home, end, p, dri->vnum, m);
	}
	pane_notify("doc:replaced", ci->home, ci->num, start, NULL,
		0, end);
	mark_free(start);
	mark_free(end);
	return 1;
}

DEF_CMD(dr_notify_close)
{
	/* Document is going away, so must I */
	pane_close(ci->home);
	return 1;
}

DEF_CMD(dr_render_line)
{
	struct mark *m = ci->mark;
	struct mark *m2 = ci->mark2;
	struct mark *mt;
	struct doc *d = ci->home->data;
	struct dr_info *dri = container_of(d, struct dr_info, doc);
	char *line;
	int ret;

	if (!m)
		return Enoarg;
	if (!m->ref.m)
		return Einval;
	if (ci->num == -1 && !m2)
		return Enoarg;
	line = m->ref.m->mdata;
	if (!line) {
		struct mark *tmp = mark_dup(m->ref.m);
		if (tmp)
			m->ref.m->mdata =
				call_ret(str, "doc:render-line", dri->base,
					 NO_NUMERIC, tmp);
		mark_free(tmp);
		if (!m->ref.m->mdata)
			m->ref.m->mdata = strdup("");
		line = m->ref.m->mdata;
	}
	if (!line)
		return Efail;
	if (m->ref.offset < 0)
		m->ref.offset = strlen(line)-1;
	if (ci->num == -1 && m2) {
		if (m2->ref.m == m->ref.m) {
			if (m2->ref.offset < 0)
				m2->ref.offset = strlen(line)-1;
			line = strndup(line, m2->ref.offset);
			ret = comm_call(ci->comm2, "callback:doc", ci->focus,
					0, NULL, line);
			free(line);
			m->ref.offset = m2->ref.offset;
			reposition_mark(m);
			return ret;
		}
		if (m2->seq < m->seq)
			return 1;
	}
	if (ci->num >= 0 && ci->num <= (int)strlen(line)) {
		line = strndup(line, ci->num);
		ret = comm_call(ci->comm2, "callback:doc", ci->focus,
				0, NULL, line);
		free(line);
		m->ref.offset = ci->num;
		reposition_mark(m);
		return ret;
	}
	ret = comm_call(ci->comm2, "callback:doc", ci->focus, 0, NULL, line);
	mt = mark_dup(m->ref.m);
	call("doc:render-line", dri->base, NO_NUMERIC, mt);
	set_ref_mark(ci->home, m, dri->base, dri->vnum, mt);
	mark_free(mt);
	return ret;
}

DEF_CMD(dr_render_prev)
{
	struct mark *m = ci->mark;
	struct mark *mt;
	struct doc *d = ci->home->data;
	struct dr_info *dri = container_of(d, struct dr_info, doc);
	int ret;

	if (!m)
		return Enoarg;
	if (!m->ref.m)
		return Einval;

	if (ci->num == 0) {
		m->ref.offset = 0;
		reposition_mark(m);
		return 1;
	}
	mt = mark_dup(m->ref.m);
	ret = call("doc:render-line-prev", dri->base, 1, mt);
	if (ret > 0)
		set_ref_mark(ci->home, m, dri->base, dri->vnum, mt);
	mark_free(mt);
	return ret;
}

DEF_CMD(dr_replace)
{
	int ret;
	struct mark *m1, *m2;
	struct doc *d = ci->home->data;
	struct dr_info *dri = container_of(d, struct dr_info, doc);

	m1 = ci->mark && ci->mark->ref.m ? mark_dup(ci->mark->ref.m) : NULL;
	m2 = ci->mark2 && ci->mark2->ref.m ? mark_dup(ci->mark2->ref.m) : NULL;

	ret = home_call(dri->base,
			ci->key, ci->focus,
			ci->num, m1, ci->str,
			ci->num2, m2, ci->str2,
			ci->x, ci->y, ci->comm2);
	mark_free(m1);
	mark_free(m2);
	return ret;
}

DEF_CMD(dr_get_attr)
{
	char *attr = ci->str;
	char *val;
	struct doc *d = ci->home->data;
	struct dr_info *dri = container_of(d, struct dr_info, doc);

	if (!attr)
		return Enoarg;
	if ((val = attr_find(ci->home->attrs, attr)) != NULL)
		;
	else if (strcmp(attr, "render-default") == 0)
		val = "lines";
	else
		val = call_ret(strsave, ci->key, dri->base,
			       ci->num, NULL, ci->str);

	if (val)
		comm_call(ci->comm2, "callback:get_attr", ci->focus,
			  0, NULL, val);
	return 1;

}

DEF_CMD(dr_revisit)
{
	struct doc *d = ci->home->data;
	struct dr_info *dri = container_of(d, struct dr_info, doc);

	home_call(dri->base, ci->key, ci->focus, ci->num);
	return 1;
}

static void dr_init_map(void)
{
	if ((void*)dr_map)
		return;
	dr_map = key_alloc();
	key_add_chain(dr_map, doc_default_cmd);
	key_add(dr_map, "doc:set-ref", &dr_set_ref);
	key_add(dr_map, "doc:step", &dr_step);
	key_add(dr_map, "Close", &dr_close);
	key_add(dr_map, "doc:notify-viewers", &dr_notify_viewers);
	key_add(dr_map, "doc:replaced", &dr_notify_replace);
	key_add(dr_map, "Notify:Close", &dr_notify_close);
	key_add(dr_map, "doc:revisit", &dr_revisit);
	key_add(dr_map, "doc:render-line", &dr_render_line);
	key_add(dr_map, "doc:render-line-prev", &dr_render_prev);
	key_add(dr_map, "doc:replace", &dr_replace);
	key_add(dr_map, "get-attr", &dr_get_attr);
}

DEF_CMD(attach_dr)
{
	struct dr_info *dri;
	struct pane *p;

	dri = calloc(1, sizeof(*dri));

	p = doc_register(ci->focus, 0, &dr_handle.c, &dri->doc);
	if (!p) {
		free(dri);
		return Efail;
	}
	dri->doc.refcnt = dr_refcnt;
	dri->base = ci->focus;

	home_call(ci->focus, "doc:request:doc:notify-viewers", p);
	home_call(ci->focus, "doc:request:doc:replaced", p);
	pane_add_notify(p, ci->focus, "Notify:Close");
	home_call(ci->focus, "doc:request:Notify:Close", p);
	//call("doc:set:autoclose", p, 1);
	dri->vnum = home_call(ci->focus, "doc:add-view", p) - 1;

	return comm_call(ci->comm2, "callback:doc", p);

}

void edlib_init(struct pane *ed safe)
{
	dr_init_map();
	call_comm("global-set-command", ed, &attach_dr, 0, NULL,
		  "attach-doc-rendering");
}
