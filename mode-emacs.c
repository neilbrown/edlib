/*
 * Copyright Neil Brown ©2015 <neil@brown.name>
 * May be distributed under terms of GPLv2 - see file:COPYING
 *
 * Define some keystrokes to create an editor with an
 * "emacs" feel.
 *
 * We register an 'emacs' mode and associate keys with that
 * in the global keymap.
 */
#include <unistd.h>
#include <stdlib.h>
#include <wchar.h>
#include <fcntl.h>
#include <string.h>

#include "core.h"

REDEF_CMD(emacs_move);
REDEF_CMD(emacs_delete);

static struct move_command {
	struct command	cmd;
	char		*type;
	int		direction;
	char		*k1, *k2, *k3;
} move_commands[] = {
	{CMD(emacs_move), "Move-Char", 1,
	 "C-Chr-F", "Right", NULL},
	{CMD(emacs_move), "Move-Char", -1,
	 "C-Chr-B", "Left", NULL},
	{CMD(emacs_move), "Move-Word", 1,
	 "M-Chr-f", "M-Right", NULL},
	{CMD(emacs_move), "Move-Word", -1,
	 "M-Chr-b", "M-Left", NULL},
	{CMD(emacs_move), "Move-WORD", 1,
	 "M-Chr-F", NULL, NULL},
	{CMD(emacs_move), "Move-WORD", -1,
	 "M-Chr-B", NULL, NULL},
	{CMD(emacs_move), "Move-EOL", 1,
	 "C-Chr-E", "End", NULL},
	{CMD(emacs_move), "Move-EOL", -1,
	 "C-Chr-A", "Home", NULL},
	{CMD(emacs_move), "Move-Line", -1,
	 "C-Chr-P", "Up", NULL},
	{CMD(emacs_move), "Move-Line", 1,
	 "C-Chr-N", "Down", NULL},
	{CMD(emacs_move), "Move-File", 1,
	 "M-Chr->", "S-End", NULL},
	{CMD(emacs_move), "Move-File", -1,
	 "M-Chr-<", "S-Home", NULL},
	{CMD(emacs_move), "Move-View-Large", 1,
	 "Next", "C-Chr-V", NULL},
	{CMD(emacs_move), "Move-View-Large", -1,
	 "Prior", "M-Chr-v", NULL},

	{CMD(emacs_delete), "Move-Char", 1,
	 "C-Chr-D", "Del", "del"},
	{CMD(emacs_delete), "Move-Char", -1,
	 "C-Chr-H", "Backspace", NULL},
	{CMD(emacs_delete), "Move-Word", 1,
	 "M-Chr-d", NULL, NULL},
	{CMD(emacs_delete), "Move-Word", -1,
	 "M-C-Chr-H", "M-Backspace", NULL},
	{CMD(emacs_delete), "Move-EOL", 1,
	 "C-Chr-K", NULL, NULL},
};

REDEF_CMD(emacs_move)
{
	struct move_command *mv = container_of(ci->comm, struct move_command, cmd);
	struct pane *cursor_pane = ci->focus;
	int old_x = -1;
	struct cmd_info ci2 = {0};
	int ret = 0;

	if (!cursor_pane)
		return 0;
	old_x = cursor_pane->cx;

	ret = call3(mv->type, ci->focus, mv->direction * RPT_NUM(ci), ci->mark);
	if (!ret)
		return 0;

	if (strcmp(mv->type, "Move-View-Large") == 0 && old_x >= 0) {
		/* Might have lost the cursor - place it at top or
		 * bottom of view, but make sure it moves only in the
		 * right direction.
		 */
		int ok;
		struct mark *old_point = mark_at_point(cursor_pane,
						       ci->mark, MARK_UNGROUPED);
		ci2.focus = cursor_pane;
		ci2.key = "Mouse-event";
		ci2.str = "Move-CursorXY";
		ci2.numeric = 1;
		ci2.x = old_x;
		ci2.mark = ci->mark;
		if (mv->direction == 1)
			ci2.y = 0;
		else
			ci2.y = cursor_pane->h - 1;
		key_handle(&ci2);
		if (mv->direction == 1)
			ok = mark_ordered_not_same_pane(cursor_pane, old_point, ci->mark);
		else
			ok = mark_ordered_not_same_pane(cursor_pane, ci->mark, old_point);
		if (!ok) {
			/* Try other end of pane */
			memset(&ci2, 0, sizeof(ci2));
			ci2.focus = cursor_pane;
			ci2.key = "Mouse-event";
			ci2.str = "Move-CursorXY";
			ci2.numeric = 1;
			ci2.x = old_x;
			ci2.mark = ci->mark;
			if (mv->direction != 1)
				ci2.y = 0;
			else
				ci2.y = cursor_pane->h - 1;
			key_handle(&ci2);
		}
		mark_free(old_point);
	}

	pane_damaged(cursor_pane, DAMAGED_CURSOR);

	return ret;
}

REDEF_CMD(emacs_delete)
{
	struct move_command *mv = container_of(ci->comm, struct move_command, cmd);
	int ret = 0;
	struct mark *m;

	m = mark_dup(ci->mark, 1);

	if (strcmp(mv->type, "Move-EOL") == 0 &&
	    mv->direction == 1 && RPT_NUM(ci) == 1 &&
	    doc_following_pane(ci->focus, m) == '\n')
		ret = call3("Move-Char", ci->focus, mv->direction * RPT_NUM(ci), m);
	else
		ret = call3(mv->type, ci->focus, mv->direction * RPT_NUM(ci), m);

	if (!ret) {
		mark_free(m);
		return 0;
	}
	ret = call5("Replace", ci->focus, 1, m, NULL, ci->extra);
	mark_free(m);
	pane_set_extra(ci->focus, 1);

	return ret;
}

REDEF_CMD(emacs_simple);
REDEF_CMD(emacs_simple_neg);
static struct simple_command {
	struct command	cmd;
	char		*type;
	char		*k;
} simple_commands[] = {
	{CMD(emacs_simple), "Window:next", "emCX-Chr-o"},
	{CMD(emacs_simple), "Window:prev", "emCX-Chr-O"},
	{CMD(emacs_simple), "Window:x+", "emCX-Chr-}"},
	{CMD(emacs_simple), "Window:x-", "emCX-Chr-{"},
	{CMD(emacs_simple), "Window:y+", "emCX-Chr-^"},
	{CMD(emacs_simple), "Window:close-others", "emCX-Chr-1"},
	{CMD(emacs_simple), "Window:split-y", "emCX-Chr-2"},
	{CMD(emacs_simple), "Window:split-x", "emCX-Chr-3"},
	{CMD(emacs_simple), "Window:close", "emCX-Chr-0"},
	{CMD(emacs_simple), "Window:scale-relative", "emCX-C-Chr-="},
	{CMD(emacs_simple_neg), "Window:scale-relative", "emCX-C-Chr--"},
	{CMD(emacs_simple), "Display:refresh", "C-Chr-L"},
	{CMD(emacs_simple), "Display:new", "emCX5-Chr-2"},
	{CMD(emacs_simple), "Abort", "C-Chr-G"},
	{CMD(emacs_simple), "NOP", "M-Chr-G"},
	{CMD(emacs_simple), "NOP", "emCX-C-Chr-G"},
	{CMD(emacs_simple), "NOP", "emCX4-C-Chr-G"},
	{CMD(emacs_simple), "doc:save-file", "emCX-C-Chr-S"},
};

REDEF_CMD(emacs_simple)
{
	struct simple_command *sc = container_of(ci->comm, struct simple_command, cmd);
	struct cmd_info ci2 = {0};

	ci2.key = sc->type;
	ci2.focus = ci->focus;
	ci2.numeric = ci->numeric;
	ci2.extra = ci->extra;
	ci2.mark = ci->mark;
	return key_handle(&ci2);
}

REDEF_CMD(emacs_simple_neg)
{
	struct simple_command *sc = container_of(ci->comm, struct simple_command, cmd);
	struct cmd_info ci2 = {0};

	ci2.key = sc->type;
	ci2.focus = ci->focus;
	ci2.numeric = -RPT_NUM(ci);
	ci2.extra = ci->extra;
	ci2.mark = ci->mark;
	return key_handle(&ci2);
}

DEF_CMD(emacs_exit)
{
	struct pane *p = ci->home;

	call3("event:deactivate", p, 0, NULL);
	return 1;
}

DEF_CMD(emacs_insert)
{
	int ret;
	char *str;

	/* Key is "Chr-X" - skip 4 bytes to get X */
	str = ci->key + 4;
	ret = call5("Replace", ci->focus, 1, ci->mark, str, ci->extra);
	pane_set_extra(ci->focus, 1);

	return ret;
}

static struct {
	char *key;
	char *insert;
} other_inserts[] = {
	{"Tab", "\t"},
	{"LF", "\n"},
	{"Return", "\n"},
	{NULL, NULL}
};

DEF_CMD(emacs_insert_other)
{
	int ret;
	int i;

	for (i = 0; other_inserts[i].key; i++)
		if (strcmp(other_inserts[i].key, ci->key) == 0)
			break;
	if (other_inserts[i].key == NULL)
		return 0;

	ret = call5("Replace", ci->focus, 1, ci->mark, other_inserts[i].insert,
		    ci->extra);
	pane_set_extra(ci->focus, 0); /* A newline starts a new undo */
	return ret;
}

DEF_CMD(emacs_undo)
{
	doc_undo(ci->focus, 0);
	return 1;
}

DEF_CMD(emacs_redo)
{
	doc_undo(ci->focus, 1);
	return 1;
}

DEF_CMD(emacs_findfile)
{
	int fd;
	struct pane *p, *par;
	struct cmd_info ci2 = {0};

	if (strncmp(ci->key, "File Found", 10) != 0) {
		char *path = NULL;
		char buf[PATH_MAX];

		path = pane_attr_get(ci->focus, "filename");
		if (path) {
			strcpy(buf, path);
			path = strrchr(buf, '/');
			if (path)
				path[1] = '\0';
			path = buf;
		}

		if (!path)
			path = realpath(".", buf);
		if (!path)
			path = "/";
		p = pane_attach(ci->focus, "popup", "D2", NULL);
		if (!p)
			return 0;

		if (strncmp(ci->key, "emCX4-", 6) == 0) {
			attr_set_str(&p->attrs, "prefix",
				     "Find File Other Window: ", -1);
			attr_set_str(&p->attrs, "done-key",
				     "File Found Other Window", -1);
		} else {
			attr_set_str(&p->attrs, "prefix", "Find File: ", -1);
			attr_set_str(&p->attrs, "done-key", "File Found", -1);
		}
		call5("doc:set-name", p, 0, NULL, "Find File", 0);
		if (path)
			call5("Replace", p, 0, NULL, path, 0);

		ci2.key = "local-set-key";
		ci2.focus = pane_final_child(p);
		ci2.str = "emacs:file-complete";
		ci2.str2 = "Tab";
		key_handle(&ci2);
		return 1;
	}

	if (strcmp(ci->key, "File Found Other Window") == 0)
		p = call_pane("OtherPane", ci->focus, 0, NULL, 0);
	else
		p = call_pane("ThisPane", ci->focus, 0, NULL, 0);

	if (!p)
		return -1;

	par = p;
	/* par is the tile */
	p = pane_child(par);
	if (p)
		pane_close(p);

	fd = open(ci->str, O_RDONLY);
	if (fd >= 0) {
		p = doc_open(par, fd, ci->str);
		if (p)
			doc_attach_view(par, p, NULL);
		close(fd);
	} else
		p = doc_from_text(par, ci->str, "File not found\n");
	if (!p)
		return -1;
	pane_focus(p);
	return 1;
}

DEF_CMD(save_str)
{
	struct call_return *cr = container_of(ci->comm, struct call_return, c);
	cr->s = ci->str ? strdup(ci->str) : NULL;
	return 1;
}

DEF_CMD(emacs_file_complete)
{
	/* Extract a directory name and a basename from the document.
	 * Find a document for the directory and attach as a completing
	 * popup menu
	 */
	char *str = doc_getstr(ci->focus, NULL);
	char *d, *b, *c;
	int fd;
	struct pane *par, *pop, *docp;
	struct cmd_info ci2 = {0};
	struct call_return cr;
	int ret;

	d = str;
	while ((c = strstr(d, "//")) != NULL)
		d = c+1;
	b = strrchr(d, '/');
	if (b) {
		b += 1;
		d = strndup(d, b-d);
	} else {
		b = d;
		d = strdup(".");
	}
	fd = open(d, O_DIRECTORY|O_RDONLY);
	if (fd < 0) {
		free(d);
		free(str);
		return -1;
	}
	docp = doc_open(ci->home, fd, d);
	close(fd);
	pop = pane_attach(ci->focus, "popup", "DM1r", pane_attr_get(docp, "doc:name"));
	if (!pop)
		return -1;
	par = pane_final_child(pop);

	attr_set_str(&par->attrs, "line-format", "%+name%suffix", -1);
	attr_set_str(&par->attrs, "heading", "", -1);
	attr_set_str(&par->attrs, "done-key", "Replace", -1);
	render_attach("complete", par);
	ci2.key = "Complete:prefix";
	ci2.str = b;
	ci2.focus = pane_final_child(par);
	cr.c = save_str;
	cr.s = NULL;
	ci2.comm2 = &cr.c;
	ret = key_handle(&ci2);
	free(d);
	if (cr.s && (strlen(cr.s) <= strlen(b) && ret-1 > 1)) {
		/* We need the dropdown */
		pane_damaged(par, DAMAGED_CONTENT);
		free(str);
		free(cr.s);
		return 1;
	}
	if (cr.s) {
		/* add the extra chars from ci2.str */
		char *c = cr.s + strlen(b);

		call5("Replace", ci->focus, 1, ci->mark, c, 0);
		free(cr.s);
	}
	/* Now need to close the popup */
	pane_close(pop);
	return 1;
}

DEF_CMD(emacs_finddoc)
{
	struct pane *p, *par;
	struct cmd_info ci2 = {0};

	if (strncmp(ci->key, "Doc Found", 9) != 0) {

		p = pane_attach(ci->focus, "popup", "D2", NULL);
		if (!p)
			return 0;

		if (strncmp(ci->key, "emCX4-", 6) == 0) {
			attr_set_str(&p->attrs, "prefix",
				     "Find Document Other Window: ", -1);
			attr_set_str(&p->attrs, "done-key",
				     "Doc Found Other Window", -1);
		} else {
			attr_set_str(&p->attrs, "prefix", "Find Document: ", -1);
			attr_set_str(&p->attrs, "done-key", "Doc Found", -1);
		}
		call5("doc:set-name", p, 0, NULL, "Find Document", 0);

		ci2.key = "local-set-key";
		ci2.focus = p;
		ci2.str = "emacs:doc-complete";
		ci2.str2 = "Tab";
		key_handle(&ci2);
		return 1;
	}

	if (strcmp(ci->key, "Doc Found Other Window") == 0)
		p = call_pane("OtherPane", ci->focus, 0, NULL, 0);
	else
		p = call_pane("ThisPane", ci->focus, 0, NULL, 0);
	if (!p)
		return -1;

	par = p;
	/* par is the tile */

	p = call_pane7("docs:byname", ci->focus, 0, NULL, 0, ci->str);
	if (!p)
		return 1;
	if (par->focus)
		pane_close(par->focus);
	p = doc_attach_view(par, p, NULL);
	return !!p;
}

DEF_CMD(emacs_doc_complete)
{
	/* Extract a document from the document.
	 * Attach the 'docs' document as a completing popup menu
	 */
	char *str = doc_getstr(ci->focus, NULL);
	struct pane *par, *pop;
	struct cmd_info ci2 = {0};
	struct call_return cr;
	int ret;

	pop = pane_attach(ci->focus, "popup", "DM1r", "*Documents*");
	if (!pop)
		return -1;
	par = pane_final_child(pop);

	attr_set_str(&par->attrs, "line-format", "%+name", -1);
	attr_set_str(&par->attrs, "heading", "", -1);
	attr_set_str(&par->attrs, "done-key", "Replace", -1);
	render_attach("complete", par);
	ci2.key = "Complete:prefix";
	ci2.str = str;
	ci2.focus = pane_final_child(par);
	cr.c = save_str;
	cr.s = NULL;
	ci2.comm2= &cr.c;
	ret = key_handle(&ci2);
	if (cr.s && (strlen(cr.s) <= strlen(str) && ret - 1 > 1)) {
		/* We need the dropdown */
		pane_damaged(par, DAMAGED_CONTENT);
		free(str);
		free(cr.s);
		return 1;
	}
	if (cr.s) {
		/* add the extra chars from cr.s */
		char *c = cr.s + strlen(str);

		call5("Replace", ci->focus, 1, ci->mark, c, 0);
		free(cr.s);
	}
	/* Now need to close the popup */
	pane_close(pop);
	return 1;
}

DEF_CMD(emacs_viewdocs)
{
	struct pane *p, *par;
	struct pane *docs;

	par = call_pane("ThisPane", ci->focus, 0, NULL, 0);
	if (!par)
		return -1;
	/* par is the tile */

	docs = call_pane7("docs:byname", ci->focus, 0, NULL, 0, "*Documents*");
	if (!docs)
		return 1;
	p = pane_child(par);
	if (p)
		pane_close(p);
	p = doc_attach_view(par, docs, NULL);
	return !!p;
}

DEF_CMD(emacs_meta)
{
	pane_set_mode(ci->focus, "M-");
	pane_set_numeric(ci->focus, ci->numeric);
	pane_set_extra(ci->focus, ci->extra);
	return 1;
}

DEF_CMD(emacs_num)
{
	int rpt = RPT_NUM(ci);
	char *last = ci->key + strlen(ci->key)-1;

	if (ci->numeric == NO_NUMERIC)
		rpt = 0;
	rpt = rpt * 10 + *last - '0';
	pane_set_numeric(ci->focus, rpt);
	pane_set_extra(ci->focus, ci->extra);
	return 1;
}

DEF_CMD(emacs_kill_doc)
{
	return call3("doc:destroy", ci->home, 0, 0);
}

DEF_CMD(emacs_search)
{
	struct cmd_info ci2 = {0};
	struct mark *m;
	int ret;

	if (strcmp(ci->key, "Search String") != 0) {
		struct pane *p = pane_attach(ci->focus, "popup", "TR2", NULL);

		if (!p)
			return 0;

		attr_set_str(&p->attrs, "prefix", "Search: ", -1);
		attr_set_str(&p->attrs, "done-key", "Search String", -1);

		call5("doc:set-name", p, 0, NULL, "Search", 0);

		p = pane_final_child(p);
		pane_attach(p, "emacs-search", NULL, NULL);
		return 1;
	}

	if (!ci->str || !ci->str[0])
		return -1;

	m = mark_at_point(ci->focus, NULL, MARK_UNGROUPED);

	ci2.key = "global-set-attr";
	ci2.str = "Search String";
	ci2.str2 = ci->str;
	ci2.focus = ci->focus;
	key_handle(&ci2);

	memset(&ci2, 0, sizeof(ci2));
	ci2.focus = ci->focus;
	ci2.mark = m;
	ci2.str = ci->str;
	ci2.key = "text-search";
	ret = key_handle(&ci2);
	if (ret > 1)
		call3("Move-to", ci->focus, 0, m);

	mark_free(m);
	return 1;
}

static struct map *emacs_map;

static void emacs_init(void)
{
	unsigned i;
	struct command *cx_cmd = key_register_prefix("emCX-");
	struct command *cx4_cmd = key_register_prefix("emCX4-");
	struct command *cx5_cmd = key_register_prefix("emCX5-");
	struct map *m = key_alloc();

	key_add(m, "C-Chr-X", cx_cmd);
	key_add(m, "emCX-Chr-4", cx4_cmd);
	key_add(m, "emCX-Chr-5", cx5_cmd);
	key_add(m, "ESC", &emacs_meta);

	for (i = 0; i < ARRAY_SIZE(move_commands); i++) {
		struct move_command *mc = &move_commands[i];
		key_add(m, mc->k1, &mc->cmd);
		if (mc->k2)
			key_add(m, mc->k2, &mc->cmd);
		if (mc->k3)
			key_add(m, mc->k3, &mc->cmd);
	}

	for (i = 0; i < ARRAY_SIZE(simple_commands); i++) {
		struct simple_command *sc = &simple_commands[i];
		key_add(m, sc->k, &sc->cmd);
	}

	key_add_range(m, "Chr- ", "Chr-~", &emacs_insert);
	key_add_range(m, "Chr-\200", "Chr-\377\377\377\377", &emacs_insert);
	key_add(m, "Tab", &emacs_insert_other);
	key_add(m, "LF", &emacs_insert_other);
	key_add(m, "Return", &emacs_insert_other);

	key_add(m, "C-Chr-_", &emacs_undo);
	key_add(m, "M-C-Chr-_", &emacs_redo);

	key_add(m, "emCX-C-Chr-F", &emacs_findfile);
	key_add(m, "emCX4-C-Chr-F", &emacs_findfile);
	key_add(m, "emCX4-Chr-f", &emacs_findfile);
	key_add(m, "File Found", &emacs_findfile);
	key_add(m, "File Found Other Window", &emacs_findfile);

	key_add(m, "emCX-Chr-b", &emacs_finddoc);
	key_add(m, "emCX4-Chr-b", &emacs_finddoc);
	key_add(m, "Doc Found", &emacs_finddoc);
	key_add(m, "Doc Found Other Window", &emacs_finddoc);
	key_add(m, "emCX-C-Chr-B", &emacs_viewdocs);

	key_add(m, "emCX-Chr-k", &emacs_kill_doc);

	key_add(m, "C-Chr-S", &emacs_search);
	key_add(m, "Search String", &emacs_search);

	key_add(m, "emCX-C-Chr-C", &emacs_exit);

	key_add_range(m, "M-Chr-0", "M-Chr-9", &emacs_num);
	emacs_map = m;
}

DEF_LOOKUP_CMD(mode_emacs, emacs_map);

void emacs_search_init(struct pane *ed);
void edlib_init(struct pane *ed)
{
	if (emacs_map == NULL)
		emacs_init();
	call_comm("global-set-command", ed, 0, NULL, "mode-emacs",
		  0, &mode_emacs.c);
	call_comm("global-set-command", ed, 0, NULL, "emacs:file-complete",
		  0, &emacs_file_complete);
	call_comm("global-set-command", ed, 0, NULL, "emacs:doc-complete",
		  0, &emacs_doc_complete);
	emacs_search_init(ed);
}
