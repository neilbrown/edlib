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
	/* If skip > 0, then the first 'skip' chars on each line
	 * are skipped over.
	 * If 'choose' is also > 0 then the whole line is skipped
	 * unless:
	 *  choose <= skip and the "choose"th char is not '+'
	 *  choose > skip and none of the skip chars are '-'
	 */
	int toskip = skip;
	bool chosen = choose == 0 || choose > skip;

	while ((!end || mark_ordered_not_same(m, end)) &&
	       (toskip || !chosen)) {
		/* Don't want this char */
		wint_t wch = doc_next(p, m);
		if (wch == WEOF)
			break;
		if (is_eol(wch)) {
			toskip = skip;
			chosen = choose == 0 || choose > skip;
		} else if (toskip) {
			toskip -= 1;
			if (choose > skip && wch == '-')
				chosen = False;
			if (skip - toskip == choose && wch != '+')
				chosen = True;
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
		if (is_eol(wch)) {
			doskip(p, m, end, skip, choose);
			if (!mark_ordered_not_same(m, end))
				break;
		}
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
	struct wtxt {
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

static void forward_lines(struct pane *p safe, struct mark *m safe,
			  int skip, int choose, int lines)
{
	while (lines > 0) {
		doskip(p, m, NULL, skip, choose);
		call("Move-EOL", p, 1, m);
		doc_next(p, m);
		lines -= 1;
	}
}

DEF_CMD(wiggle_text)
{
	/* remember pane, mark1, mark2, num,  num2 */
	struct wiggle_data *wd = ci->home->data;
	struct mark *m2;
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

	if (!ci->mark || (!ci->mark2 && !ci->str))
		return Enoarg;
	if (ci->num < 0 || ci->num2 < 0 || ci->num2 > ci->num+1)
		return Einval;
	if (!ci->mark2) {
		int lines = atoi(ci->str ?: "1");
		m2 = mark_dup(ci->mark);
		forward_lines(ci->focus, m2, ci->num, ci->num2, lines);
	} else
		m2 = mark_dup(ci->mark2);

	wd->texts[which].text = ci->focus;
	pane_add_notify(ci->home, ci->focus, "Notify:Close");
	wd->texts[which].start = mark_dup(ci->mark);
	wd->texts[which].end = m2;
	wd->texts[which].skip = ci->num;
	wd->texts[which].choose = ci->num2;

	return 1;
}

DEF_CMD(wiggle_extract)
{
	struct wiggle_data *wd = ci->home->data;
	struct wtxt *wt;
	struct stream str;

	if (!ci->str || !ci->comm2)
		return Enoarg;
	if (strcmp(ci->str, "orig") == 0)
		wt = &wd->texts[0];
	else if (strcmp(ci->str, "before") == 0)
		wt = &wd->texts[1];
	else if (strcmp(ci->str, "after") == 0)
		wt = &wd->texts[2];
	else
		return Einval;
	if (!collect(wt->text, wt->start, wt->end, wt->skip, wt->choose,
		     &str))
		return Enoarg;

	comm_call(ci->comm2, "cb", ci->focus, 0, NULL, str.body);
	free(str.body);
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

DEF_CMD(wiggle_find)
{
	/* Find orig, before or after in 'focus' near 'mark'
	 * str in "orig", "before" or "after"
	 * num is max number of lines to stripe (fuzz)
	 * num2 is max number of lines, defaults to searching whole file.
	 * Returns number of fuzz lines, plus 1
	 */
	struct wiggle_data *wd = ci->home->data;
	int lines = ci->num2;
	struct pane *p = ci->focus;
	struct stream str;
	char *match, *end;
	struct wtxt *wt;
	int ret = Efail;
	int fuzz = 0;

	if (!ci->mark || !ci->str)
		return Enoarg;
	if (strcmp(ci->str, "orig") == 0)
		wt = &wd->texts[0];
	else if (strcmp(ci->str, "before") == 0)
		wt = &wd->texts[1];
	else if (strcmp(ci->str, "after") == 0)
		wt = &wd->texts[2];
	else
		return Einval;
	if (!collect(wt->text, wt->start, wt->end, wt->skip, wt->choose,
		     &str))
		return Enoarg;

	match = str.body;
	if (!match)
		return Enoarg;
	do {
		struct mark *early, *late;

		early = mark_dup(ci->mark);
		call("Move-EOL", p, -1, early);
		late = mark_dup(ci->mark);
		call("Move-EOL", p, 1, late);
		if (call("Move-Char", p, 1, late) < 0) {
			mark_free(late);
			late = NULL;
		}

		while (early || late) {
			if (early) {
				ret = call("text-equals", p, 0, early, match);
				if (ret > 0) {
					mark_to_mark(ci->mark, early);
					break;
				}
				if (ret != Efalse || doc_prior(p, early) == WEOF) {
					mark_free(early);
					early = NULL;
				} else
					call("Move-EOL", p, -2, early);
			}
			if (late) {
				ret = call("text-equals", p, 0, late, match);
				if (ret > 0) {
					mark_to_mark(ci->mark, late);
					break;
				}
				if (ret != Efalse || doc_following(p, late) == WEOF) {
					mark_free(late);
					late = NULL;
				} else {
					call("Move-EOL", p, 1, late);
					doc_next(p, late);
				}
			}
			if (lines > 0) {
				lines -= 1;
				if (lines == 0)
					break;
			}
		}
		mark_free(early);
		mark_free(late);

		if (ret > 0)
			break;
		fuzz += 1;
		match = strchr(match, '\n');
		if (!match)
			break;
		match += 1;
		end = strrchr(match, '\n');
		if (end)
			*end = 0;
		end = strrchr(match, '\n');
		if (end)
			end[1] = 0;
		else
			break;
	} while (fuzz < ci->num);

	free(str.body);
	return ret > 0 ? fuzz + 1 : Efail;
}

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
		key_add(wiggle_map, "extract", &wiggle_extract);
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
