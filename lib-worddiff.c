/*
 * Copyright Neil Brown ©2020 <neil@brown.name>
 * May be distributed under terms of GPLv2 - see file:COPYING
 *
 * worddiff - mark word-wise differences in two ranges.
 *
 * ranges currently have to be in same file.
 * We use code from wiggle which requires that a 'stream'
 * (a char[] with length) be split as words into a 'file', then two
 * such 'file's passed to 'diff()' to produce a 'csl' (common subsequence list).
 * The a/b/len of each element in the csl are index to the respective
 * files, which have indexes into the streams.
 */

#include <wctype.h>
#include "core.h"
#include "misc.h"
#include "wiggle/wiggle.h"

/* lib-wiggle currently needs these */
void *xmalloc(int size)
{
	return malloc(size);
}
int do_trace = 0;
void printword(FILE *f, struct elmnt e)
{
}

static bool has_nonspace(const char *s, int len)
{
	wint_t ch;
	const char *end = s+len;

	while ((ch = get_utf8(&s, end)) != WEOF &&
	       ch != WERR &&
	       iswspace(ch))
		;
	return ch != WEOF;
}

static bool only_spaces(struct file f, struct csl *csl safe, int which)
{
	int fpos = 0;

	if (!f.list)
		return True;

	for (; csl->len; csl += 1) {
		int o = which ? csl->b : csl->a;

		for (; fpos < o; fpos += 1) {
			struct elmnt e = f.list[fpos];

			if (has_nonspace(e.start, e.len))
				return False;
		}
		fpos = o + csl->len;
	}
	for (; fpos < f.elcnt; fpos += 1) {
		struct elmnt e = f.list[fpos];

		if (has_nonspace(e.start, e.len))
			return False;
	}
	return True;
}

static void doskip(struct pane *p safe,
		   struct mark *m safe, struct mark *end,
		   int skip, int choose)
{
	int toskip = skip;
	bool chosen = choose == 0;

	while ((!end || mark_ordered_not_same(m, end)) &&
	       (toskip || !chosen)) {
		/* Don't want this char */
		wint_t wch = doc_next(p, m);
		if (wch == WEOF)
			break;
		if (is_eol(wch)) {
			toskip = skip;
			chosen = choose == 0;
		} else if (toskip) {
			if (toskip == choose && wch != ' ')
				chosen = True;
			toskip -= 1;
		}
	}
}

static bool collect(struct pane *p, struct mark *start, struct mark *end,
		    int skip, int choose, struct stream *s safe)
{
	struct mark *m;
	wint_t wch = '\n';
	struct buf b;

	if (!p || !start || !end)
		return False;

	buf_init(&b);
	m = mark_dup(start);
	while (mark_ordered_not_same(m, end)) {
		if (is_eol(wch))
			doskip(p, m, end, skip, choose);
		wch = doc_next(p, m);
		if (wch == WEOF)
			break;
		buf_append(&b, wch);
	}
	s->body = buf_final(&b);
	s->len = b.len;
	mark_free(m);

	return True;
}

static void add_markup(struct pane *p, struct mark *start,
		       int skip, int choose,
		       struct stream astream, struct file afile,
		       struct csl *csl safe, const char *attr safe, int which)
{
	/* Each range of characters that is mentioned in csl gets an attribute
	 * named 'attr' with value 'len' from csl.
	 * If a range crosses a newline, the first (non-skipped) character
	 * also gets the attribute with the remaining length.
	 */
	const char *pos = astream.body;
	struct mark *m;
	wint_t ch = '\n';

	if (!p || !start || !afile.list || !pos)
		return;
	m = mark_dup(start);
	while (csl->len) {
		int st = which ? csl->b : csl->a;
		const char *startp = afile.list[st].start;
		const char *endp =	afile.list[st + csl->len - 1].start +
					afile.list[st + csl->len - 1].len;
		char buf[20];
		int len;

		if (is_eol(ch))
			doskip(p, m, NULL, skip, choose);
		while (pos < startp) {
			get_utf8(&pos, NULL);
			ch = doc_next(p, m);
			if (is_eol(ch))
				doskip(p, m, NULL, skip, choose);
		}
		/* Convert csl->len in bytes to len in codepoints. */
		len = 0;
		while (pos < endp) {
			get_utf8(&pos, NULL);
			len += 1;
		}
		pos = startp;
		snprintf(buf, sizeof(buf), "%d %d", len, which);
		call("doc:set-attr", p, 0, m, attr, 0, NULL, buf);
		ch = ' ';
		while (pos < endp) {
			get_utf8(&pos, NULL);
			if (is_eol(ch)) {
				doskip(p, m, NULL, skip, choose);
				snprintf(buf, sizeof(buf), "%d %d", len, which);
				call("doc:set-attr", p, 0, m, attr,
				     0, NULL, buf);
			}
			len -= 1;
			ch = doc_next(p, m);
		}
		csl += 1;
	}
	mark_free(m);
}

/* We provide a command that handles wiggling across multiple panes.  It
 * is paired with a private pane which can get notifications when those
 * panes are closed.
 */
struct wiggle_data {
	struct pane *private safe;
	struct {
		struct pane *text;
		struct mark *start, *end;
		short skip;	/* prefix chars to skip */
		short choose;	/* if !=0, only chose lines with non-space
				 * in this position 1..skip
				 */
	} texts[3];
	struct command c;
};

DEF_CMD(notify_close)
{
	/* Private pane received a "close" notification. */
	struct wiggle_data *wd = ci->home->data;
	int i;

	for (i = 0; i < 3; i++)
		if (ci->focus == wd->texts[i].text ||
		    ci->focus == wd->private) {
			mark_free(wd->texts[i].start);
			wd->texts[i].start = NULL;
			mark_free(wd->texts[i].end);
			wd->texts[i].end = NULL;
			wd->texts[i].text = NULL;
		}
	return 1;
}

DEF_CMD(wiggle_close)
{
	struct wiggle_data *wd = ci->home->data;
	int i;

	for (i = 0; i < 3 ; i++) {
		mark_free(wd->texts[i].start);
		wd->texts[i].start = NULL;
		mark_free(wd->texts[i].end);
		wd->texts[i].end = NULL;
		wd->texts[i].text = NULL;
	}
	return 1;
}

static void wiggle_free(struct command *c safe)
{
	struct wiggle_data *wd = container_of(c, struct wiggle_data, c);

	pane_close(wd->private);
}

DEF_CB(do_wiggle)
{
	struct wiggle_data *wd = container_of(ci->comm, struct wiggle_data, c);

	return home_call(wd->private, ci->key, ci->focus,
			 ci->num, ci->mark, ci->str,
			 ci->num2, ci->mark2, ci->str2,
			 ci->x, ci->y, ci->comm2);
}

DEF_CMD(wiggle_text)
{
	/* remember pane, mark1, mark2, num,  num2 */
	struct wiggle_data *wd = ci->home->data;
	char k0 = ci->key[0];
	int which = k0 == 'b' ? 1 : k0 == 'a' ? 2 : 0;

	/* Always clean out, even it not given enough args */
	mark_free(wd->texts[which].start);
	wd->texts[which].start = NULL;
	mark_free(wd->texts[which].end);
	wd->texts[which].end = NULL;
	/* It isn't possible to drop individual notificartion links.
	 * We will lose them all on close, and ignore any before that.
	 */
	wd->texts[which].text = NULL;

	if (!ci->mark || !ci->mark2)
		return Enoarg;
	if (ci->num < 0 || ci->num2 < 0 || ci->num2 > ci->num)
		return Einval;

	wd->texts[which].text = ci->focus;
	pane_add_notify(ci->home, ci->focus, "Notify:Close");
	wd->texts[which].start = mark_dup(ci->mark);
	wd->texts[which].end = mark_dup(ci->mark2);
	wd->texts[which].skip = ci->num;
	wd->texts[which].choose = ci->num2;

	return 1;
}

DEF_CMD(wiggle_set_common)
{
	/* Set the attribute 'str' on all common ranges in
	 * 'before' and 'after'
	 */
	struct wiggle_data *wd = ci->home->data;
	const char *attr = ci->str ?: "render:common";
	struct stream before, after;
	struct file bfile, afile;
	struct csl *csl;
	int ret = Efail;

	if (!collect(wd->texts[1].text, wd->texts[1].start, wd->texts[1].end,
		     wd->texts[1].skip, wd->texts[1].choose, &before))
		return Enoarg;
	if (!collect(wd->texts[2].text, wd->texts[2].start, wd->texts[2].end,
		     wd->texts[2].skip, wd->texts[2].choose, &after)) {
		free(before.body);
		return Enoarg;
	}

	bfile = split_stream(before, ByWord);
	afile = split_stream(after, ByWord);
	csl = diff(bfile, afile);
	if (csl) {
		add_markup(wd->texts[1].text, wd->texts[1].start,
			   wd->texts[1].skip, wd->texts[1].choose,
			   before, bfile, csl, attr, 0);
		add_markup(wd->texts[2].text, wd->texts[2].start,
			   wd->texts[2].skip, wd->texts[2].choose,
			   after, afile, csl, attr, 1);

		ret = 3;
		if (only_spaces(bfile, csl, 0) &&
		    only_spaces(afile, csl, 1))
			ret = 2; /* only space differences */
	}

	free(bfile.list);
	free(afile.list);
	free(csl);
	free(before.body);
	free(after.body);

	return ret;
}

static char *typenames[] = {
	[End] = "End",
	[Unmatched] = "Unmatched",
	[Unchanged] = "Unchanged",
	[Extraneous] = "Extraneous",
	[Changed] = "Changed",
	[Conflict] = "Conflict",
	[AlreadyApplied] = "AlreadyApplied",
};

static void add_merge_markup(struct pane *p safe,
			     struct mark *st,
			     int skip, int choose,
			     struct file f, struct merge *merge safe,
			     const char *attr safe, int which)
{
	struct merge *m;
	int pos = 0;

	if (!f.list || !st)
		return;

	doskip(p, st, NULL, skip, choose);
	for (m = merge; m->type != End; m++) {
		int len;
		const char *cp, *endcp;
		int chars;
		char buf[30];

		switch (which) {
		case 0: /* orig - no Extraneous */
			if (m->type == Extraneous)
				continue;
			if (pos != m->a) abort();
			len = m->al;
			break;
		case 1: /* before - no Unmatched */
			if (m->type == Unmatched)
				continue;
			if (pos != m->b) abort();
			len = m->bl;
			break;
		case 2: /* after - no Unmatched */
			if (m->type == Unmatched)
				continue;
			if (pos != m->c) abort();
			len = m->cl;
			break;
		}
		/* From here for 'len' element in f are 'm->type' */
		if (!len)
			continue;
		cp = f.list[pos].start;
		endcp = f.list[pos+len-1].start + f.list[pos+len-1].len;
		pos += len;
		chars = 0;
		while (get_utf8(&cp, endcp) != WEOF)
			chars += 1;

		snprintf(buf, sizeof(buf), "%d %s", chars, typenames[m->type]);
		call("doc:set-attr", p, 0, st, attr, 0, NULL, buf);
		while (chars > 0) {
			wint_t ch = doc_next(p, st);

			if (ch == WEOF)
				break;
			if (is_eol(ch))
				doskip(p, st, NULL, skip, choose);
			chars -= 1;
			if (is_eol(ch) && chars > 0) {
				snprintf(buf, sizeof(buf), "%d %s", chars,
					 typenames[m->type]);
				call("doc:set-attr", p, 0, st, attr,
				     0, NULL, buf);
			}
		}
	}
}

DEF_CMD(wiggle_set_wiggle)
{
	struct wiggle_data *wd = ci->home->data;
	struct stream ostr, astr, bstr;
	struct file of, af, bf;
	struct csl *csl1, *csl2;
	struct ci info;
	const char *attr = ci->str ?: "render:wiggle";

	if (!collect(wd->texts[0].text, wd->texts[0].start, wd->texts[0].end,
		     wd->texts[0].skip, wd->texts[0].choose, &ostr))
		return Enoarg;
	if (!collect(wd->texts[1].text, wd->texts[1].start, wd->texts[1].end,
		     wd->texts[1].skip, wd->texts[1].choose, &bstr)) {
		free(ostr.body);
		return Enoarg;
	}
	if (!collect(wd->texts[2].text, wd->texts[2].start, wd->texts[2].end,
		     wd->texts[2].skip, wd->texts[2].choose, &astr)) {
		free(ostr.body);
		free(bstr.body);
		return Enoarg;
	}

	of = split_stream(ostr, ByWord);
	bf = split_stream(bstr, ByWord);
	af = split_stream(astr, ByWord);

	csl1 = diff(of, bf);
	csl2 = diff(bf, af);
	info = make_merger(of, bf, af, csl1, csl2, 1, 1, 0);
	if (info.merger) {
		add_merge_markup(ci->focus,
				 wd->texts[0].start,
				 wd->texts[0].skip, wd->texts[0].choose,
				 of, info.merger, attr, 0);
		add_merge_markup(ci->focus,
				 wd->texts[1].start,
				 wd->texts[1].skip, wd->texts[1].choose,
				 bf, info.merger, attr, 1);
		add_merge_markup(ci->focus,
				 wd->texts[2].start,
				 wd->texts[2].skip, wd->texts[2].choose,
				 af, info.merger, attr, 2);
	}

	free(csl1);
	free(csl2);
	free(of.list);
	free(bf.list);
	free(af.list);
	free(ostr.body);
	free(bstr.body);
	free(astr.body);

	return info.conflicts + 1;
}

DEF_CMD(wiggle_find) { return 0; }
DEF_CMD(wiggle_find_best) { return 0; }
DEF_CMD(wiggle_wiggle) { return 0; }

static struct map *wiggle_map;
DEF_LOOKUP_CMD(wiggle_pane, wiggle_map);

DEF_CMD(make_wiggle)
{
	struct wiggle_data *wd;
	struct pane *p;

	if (!wiggle_map) {
		wiggle_map = key_alloc();
		key_add(wiggle_map, "Notify:Close", &notify_close);
		key_add(wiggle_map, "Close", &wiggle_close);
		key_add(wiggle_map, "orig", &wiggle_text);
		key_add(wiggle_map, "before", &wiggle_text);
		key_add(wiggle_map, "after", &wiggle_text);
		key_add(wiggle_map, "set-common", &wiggle_set_common);
		key_add(wiggle_map, "set-wiggle", &wiggle_set_wiggle);
		key_add(wiggle_map, "find", &wiggle_find);
		key_add(wiggle_map, "find-best", &wiggle_find_best);
		key_add(wiggle_map, "wiggle", &wiggle_wiggle);
	}

	alloc(wd, pane);
	wd->c = do_wiggle;
	wd->c.free = wiggle_free;
	p = pane_register(pane_root(ci->focus), 0,
			  &wiggle_pane.c, wd);
	if (!p) {
		unalloc(wd, pane);
		return Efail;
	}
	command_get(&wd->c);
	wd->private = p;
	comm_call(ci->comm2, "cb", ci->focus,
		  0, NULL, NULL,
		  0, NULL, NULL, 0,0, &wd->c);
	command_put(&wd->c);
	return 1;
}

void edlib_init(struct pane *ed safe)
{
	call_comm("global-set-command", ed, &make_wiggle,
		  0, NULL, "MakeWiggle");
}
