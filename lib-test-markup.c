/*
 * Copyright Neil Brown ©2023 <neil@brown.name>
 * May be distributed under terms of GPLv2 - see file:COPYING
 *
 * This is a replacement for lib-markup which uses each line
 * of the document as verbatim markup.  This is for testing only.
 */

#include "core.h"
#include "misc.h"

DEF_CMD(test_render_prev)
{
	struct mark *m = ci->mark;

	if (!m)
		return Enoarg;
	if (ci->num)
		if (doc_prev(ci->focus, m) == WEOF)
			return Efail;
	call("doc:EOL", ci->focus, -1, m);
	return 1;
}

DEF_CMD(test_render_line)
{
	struct mark *m = ci->mark;
	struct mark *st;
	char *s;
	int ret;
	int pm_offset = -1;

	if (!m)
		return Enoarg;
	st = mark_dup(m);
	call("doc:EOL", ci->focus, 1, m, NULL, 1);
	s = call_ret(str, "doc:get-str", ci->focus, 0, st, NULL, 0, m);
	if (ci->num >= 0) {
		struct mark *m2 = mark_dup(st);
		call("doc:char", ci->focus, ci->num, m2, NULL, 0, m);
		mark_to_mark(m, m2);
		mark_free(m2);
		if (s && ci->num < (int)utf8_strlen(s))
			s[ci->num] = 0;
	}
	if (ci->mark2) {
		char *s2 = call_ret(str, "doc:get-str", ci->focus,
				    0, st, NULL,
				    0, ci->mark2);
		pm_offset = s2 ? strlen(s2) : 0;
		free(s2);
	}
	ret = comm_call(ci->comm2, "cb", ci->focus, pm_offset, NULL, s);
	free(s);
	return ret ?: 1;
}

static struct map *tmu_map safe;
DEF_LOOKUP_CMD(test_markup_handle, tmu_map);

DEF_CMD(test_attach)
{
	struct pane *ret;

	ret = pane_register(ci->focus, 0, &test_markup_handle.c);
	if (!ret)
		return Efail;
	return comm_call(ci->comm2, "cb", ret);
}

DEF_CMD(test_enable)
{
	call("attach-test-markup", ci->focus);
	return 1;
}

void edlib_init(struct pane *ed safe)
{
	tmu_map = key_alloc();

	key_add(tmu_map, "doc:render-line", &test_render_line);
	key_add(tmu_map, "doc:render-line-prev", &test_render_prev);

	call_comm("global-set-command", ed, &test_attach,
		  0, NULL, "attach-test-markup");
	call_comm("global-set-command" ,ed, &test_enable,
		  0, NULL, "interactive-cmd-test-markup");
}
