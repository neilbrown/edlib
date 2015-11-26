/*
 * Copyright Neil Brown <neil@brown.name> 2015
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
	struct pane *cursor_pane = pane_with_cursor(ci->home, NULL, NULL);
	struct point *pt = *ci->pointp;
	int old_x = -1;
	struct cmd_info ci2 = {0};
	int ret = 0;

	if (!cursor_pane)
		return 0;
	old_x = cursor_pane->cx;

	ci2.focus = ci->focus;
	ci2.key = mv->type;
	ci2.numeric = mv->direction * RPT_NUM(ci);
	ci2.mark = &pt->m;
	ci2.pointp = ci->pointp;
	ret = key_handle_focus(&ci2);

	if (!ret)
		return 0;

	if (strcmp(mv->type, "Move-View-Large") == 0 && old_x >= 0) {
		/* Might have lost the cursor - place it at top or
		 * bottom of view
		 */
		ci2.focus = cursor_pane;
		ci2.key = "Move-CursorXY";
		ci2.numeric = 1;
		ci2.x = old_x;
		if (mv->direction == 1)
			ci2.y = 0;
		else
			ci2.y = cursor_pane->h - 1;
		ci2.pointp = ci->pointp;
		key_handle_xy(&ci2);
	}

	pane_damaged(cursor_pane, DAMAGED_CURSOR);

	return ret;
}

REDEF_CMD(emacs_delete)
{
	struct move_command *mv = container_of(ci->comm, struct move_command, cmd);
	struct cmd_info ci2 = {0};
	int ret = 0;
	struct mark *m;
	struct doc *d = ci->pointp[0]->doc;

	m = mark_at_point(*ci->pointp, MARK_UNGROUPED);
	ci2.focus = ci->focus;
	ci2.key = mv->type;
	ci2.numeric = mv->direction * RPT_NUM(ci);
	if (strcmp(mv->type, "Move-EOL") == 0 && ci2.numeric == 1 &&
	    doc_following(d, m) == '\n')
		ci2.key = "Move-Char";
	ci2.mark = m;
	ci2.pointp = ci->pointp;
	ret = key_handle_focus(&ci2);
	if (!ret) {
		mark_free(m);
		return 0;
	}
	ci2.focus = ci->focus;
	ci2.key = "Replace";
	ci2.numeric = 1;
	ci2.extra = ci->extra;
	ci2.mark = m;
	ci2.str = NULL;
	ret = key_handle_focus(&ci2);
	mark_free(m);
	pane_set_extra(ci->home, 1);

	return ret;
}

REDEF_CMD(emacs_str);
static struct str_command {
	struct command	cmd;
	char		*type;
	char		*str;
	char		*k;
} str_commands[] = {
	{CMD(emacs_str), "WindowOP", "next", "emCX-Chr-o"},
	{CMD(emacs_str), "WindowOP", "prev", "emCX-Chr-O"},
	{CMD(emacs_str), "WindowOP", "x+", "emCX-Chr-}"},
	{CMD(emacs_str), "WindowOP", "x-", "emCX-Chr-{"},
	{CMD(emacs_str), "WindowOP", "y+", "emCX-Chr-^"},
	{CMD(emacs_str), "WindowOP", "close-others", "emCX-Chr-1"},
	{CMD(emacs_str), "WindowOP", "split-y", "emCX-Chr-2"},
	{CMD(emacs_str), "WindowOP", "split-x", "emCX-Chr-3"},
	{CMD(emacs_str), "WindowOP", "close", "emCX-Chr-0"},
	{CMD(emacs_str), "Misc", "exit", "emCX-C-Chr-C"},
	{CMD(emacs_str), "Misc", "refresh", "C-Chr-L"},
	{CMD(emacs_str), "Abort", NULL, "C-Chr-G"},
	{CMD(emacs_str), "NOP", NULL, "M-Chr-G"},
	{CMD(emacs_str), "NOP", NULL, "emCX-C-Chr-G"},
	{CMD(emacs_str), "NOP", NULL, "emCX4-C-Chr-G"},
	{CMD(emacs_str), "doc:save-file", NULL, "emCX-C-Chr-S"},
};

REDEF_CMD(emacs_str)
{
	struct str_command *sc = container_of(ci->comm, struct str_command, cmd);
	struct cmd_info ci2;

	ci2 = *ci;
	ci2.key = sc->type;
	ci2.str = sc->str;
	return key_handle_focus(&ci2);
}

DEF_CMD(emacs_insert)
{
	char str[5];
	struct cmd_info ci2 = {0};
	int ret;

	ci2.focus = ci->focus;
	ci2.key = "Replace";
	ci2.numeric = 1;
	ci2.extra = ci->extra;
	ci2.mark = &(*ci->pointp)->m;
	strncpy(str,ci->key+4, sizeof(str));
	str[4] = 0;
	ci2.str = str;
	ret = key_handle_focus(&ci2);
	pane_set_extra(ci->home, 1);

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
	struct pane *p = ci->home;
	struct cmd_info ci2 = {0};
	int ret;
	int i;

	ci2.focus = ci->focus;
	ci2.key = "Replace";
	ci2.numeric = 1;
	ci2.extra = ci->extra;
	ci2.mark = &(*ci->pointp)->m;
	for (i = 0; other_inserts[i].key; i++)
		if (strcmp(other_inserts[i].key, ci->key) == 0)
			break;
	if (other_inserts[i].key == NULL)
		return 0;

	ci2.str = other_inserts[i].insert;
	ret = key_handle_focus(&ci2);
	pane_set_extra(p, 0); /* A newline starts a new undo */
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

		if (ci->pointp) {
			path = doc_attr((*ci->pointp)->doc, NULL, 0, "filename");
			if (path) {
				strcpy(buf, path);
				path = strrchr(buf, '/');
				if (path)
					path[1] = '\0';
				path = buf;
			}
		}
		if (!path)
			path = realpath(".", buf);
		if (!path)
			path = "/";
		p = pane_attach(ci->focus, "popup", NULL, "D2");
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
		ci2.key = "doc:set-name";
		ci2.focus = p;
		ci2.str = "Find File";
		key_handle_focus(&ci2);
		if (path) {
			memset(&ci2, 0, sizeof(ci2));
			ci2.key = "Replace";
			ci2.focus = p;
			ci2.str = path;
			key_handle_focus(&ci2);
		}
		memset(&ci2, 0, sizeof(ci2));
		ci2.key = "local-set-key";
		ci2.focus = p;
		ci2.str = "emacs:file-complete";
		ci2.str2 = "Tab";
		key_handle_focus(&ci2);
		return 1;
	}

	if (strcmp(ci->key, "File Found Other Window") == 0)
		ci2.key = "OtherPane";
	else
		ci2.key = "ThisPane";
	ci2.focus = ci->focus;
	if (key_handle_focus(&ci2) == 0)
		return -1;
	p = ci2.focus;

	par = p;
	/* par is the tile */
	p = pane_child(par);
	if (p)
		pane_close(p);

	fd = open(ci->str, O_RDONLY);
	if (fd >= 0) {
		p = doc_open(pane2ed(par), fd, ci->str);
		if (p)
			doc_attach_view(par, p, NULL);
		close(fd);
	} else
		p = doc_from_text(par, ci->str, "File not found\n");
	pane_focus(p);
	return 1;
}

DEF_CMD(emacs_file_complete)
{
	/* Extract a directory name and a basename from the document.
	 * Find a document for the directory and attach as a completing
	 * popup menu
	 */
	struct doc *doc = (*ci->pointp)->doc;
	char *str = doc_getstr(ci->focus, NULL);
	char *d, *b, *c;
	int fd;
	struct pane *par, *pop, *docp;
	struct cmd_info ci2 = {0};

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
	docp = doc_open(doc->ed, fd, d);
	close(fd);
	pop = pane_attach(ci->focus, "popup", docp, "DM1");
	par = pane_final_child(pop);

	attr_set_str(&par->attrs, "line-format", "%+name%suffix", -1);
	attr_set_str(&par->attrs, "heading", "", -1);
	attr_set_str(&par->attrs, "done-key", "Replace", -1);
	render_attach("complete", par);
	ci2.key = "Complete:prefix";
	ci2.str = b;
	ci2.focus = par;
	key_handle_focus(&ci2);
	free(d);
	if (ci2.str && (strlen(ci2.str) <= strlen(b) && ci2.extra > 1)) {
		/* We need the dropdown */
		pane_damaged(par, DAMAGED_CONTENT);
		free(str);
		return 1;
	}
	if (ci2.str) {
		/* add the extra chars from ci2.str */
		char *c = ci2.str + strlen(b);
		struct cmd_info ci3 = {0};
		ci3.key = "Replace";
		ci3.mark = &(*ci->pointp)->m;
		ci3.numeric = 1;
		ci3.focus = ci->focus;
		ci3.str = c;
		key_handle_focus(&ci3);
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

		p = pane_attach(ci->focus, "popup", NULL, "D2");
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
		ci2.key = "doc:set-name";
		ci2.focus = p;
		ci2.str = "Find Document";
		key_handle_focus(&ci2);

		memset(&ci2, 0, sizeof(ci2));
		ci2.key = "local-set-key";
		ci2.focus = p;
		ci2.str = "emacs:doc-complete";
		ci2.str2 = "Tab";
		key_handle_focus(&ci2);
		return 1;
	}

	if (strcmp(ci->key, "Doc Found Other Window") == 0)
		ci2.key = "OtherPane";
	else
		ci2.key = "ThisPane";
	ci2.focus = ci->focus;
	if (key_handle_focus(&ci2) == 0)
		return -1;
	p = ci2.focus;

	par = p;
	/* par is the tile */

	p = doc_find(pane2ed(par), ci->str);
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
	struct doc *doc = (*ci->pointp)->doc;
	char *str = doc_getstr(ci->focus, NULL);
	struct pane *par, *pop;
	struct cmd_info ci2 = {0};

	pop = pane_attach(ci->focus, "popup", doc->ed->docs->home, "DM1");
	par = pane_final_child(pop);

	attr_set_str(&par->attrs, "line-format", "%+name", -1);
	attr_set_str(&par->attrs, "heading", "", -1);
	attr_set_str(&par->attrs, "done-key", "Replace", -1);
	render_attach("complete", par);
	ci2.key = "Complete:prefix";
	ci2.str = str;
	ci2.focus = par;
	key_handle_focus(&ci2);
	if (ci2.str && (strlen(ci2.str) <= strlen(str) && ci2.extra > 1)) {
		/* We need the dropdown */
		pane_damaged(par, DAMAGED_CONTENT);
		free(str);
		return 1;
	}
	if (ci2.str) {
		/* add the extra chars from ci2.str */
		char *c = ci2.str + strlen(str);
		struct cmd_info ci3 = {0};
		ci3.key = "Replace";
		ci3.mark = &(*ci->pointp)->m;
		ci3.numeric = 1;
		ci3.focus = ci->focus;
		ci3.str = c;
		key_handle_focus(&ci3);
	}
	/* Now need to close the popup */
	pane_close(pop);
	return 1;
}

DEF_CMD(emacs_viewdocs)
{
	struct pane *p, *par;
	struct doc *d;
	struct cmd_info ci2 = {0};

	ci2.key = "ThisPane";
	ci2.focus = ci->focus;
	if (key_handle_focus(&ci2) == 0)
		return -1;
	par = ci2.focus;
	/* par is the tile */

	d = pane2ed(par)->docs;
	if (!d)
		return 1;
	p = pane_child(par);
	if (p)
		pane_close(p);
	p = doc_attach_view(par, d->home, NULL);
	return !!p;
}

DEF_CMD(emacs_meta)
{
	pane_set_mode(ci->home, "M-", 1);
	pane_set_numeric(ci->home, ci->numeric);
	pane_set_extra(ci->home, ci->extra);
	return 1;
}

DEF_CMD(emacs_num)
{
	int rpt = RPT_NUM(ci);
	char *last = ci->key + strlen(ci->key)-1;

	if (ci->numeric == NO_NUMERIC)
		rpt = 0;
	rpt = rpt * 10 + *last - '0';
	pane_set_numeric(ci->home, rpt);
	pane_set_extra(ci->home, ci->extra);
	return 1;
}

DEF_CMD(emacs_kill_doc)
{
	struct doc *d;
	if (!ci->pointp)
		return 0;
	d = (*ci->pointp)->doc;
	doc_destroy(d);
	return 1;
}

DEF_CMD(emacs_search)
{
	struct cmd_info ci2 = {0};
	struct mark *m;

	if (strcmp(ci->key, "Search String") != 0) {
		struct pane *p = pane_attach(ci->focus, "popup", NULL, "TR2");

		if (!p)
			return 0;

		attr_set_str(&p->attrs, "prefix", "Search: ", -1);
		attr_set_str(&p->attrs, "done-key", "Search String", -1);

		ci2.key = "doc:set-name";
		ci2.focus = p;
		ci2.str = "Search";
		key_handle_focus(&ci2);

		p = pane_final_child(p);
		pane_attach(p, "emacs-search", NULL, NULL);
		return 1;
	}

	if (!ci->str || !ci->str[0])
		return -1;

	ci2.key = "PointDup";
	ci2.focus = ci->home;
	ci2.extra = MARK_UNGROUPED;
	key_handle_focus(&ci2);
	m = ci2.mark;

	memset(&ci2, 0, sizeof(ci2));
	ci2.focus = ci->home;
	ci2.mark = m;
	ci2.str = ci->str;
	ci2.key = "text-search";
	if (!key_lookup(pane2ed(ci->focus)->commands, &ci2))
		ci2.extra = -1;
	if (ci2.extra > 0) {
		memset(&ci2, 0, sizeof(ci2));
		ci2.key = "Move-to";
		ci2.mark = m;
		ci2.focus = ci->focus;
		key_handle_focus(&ci2);
	}
	mark_free(m);
	return 1;
}

static struct map *emacs_map;

static void emacs_init(void)
{
	unsigned i;
	struct command *cx_cmd = key_register_prefix("emCX-");
	struct command *cx4_cmd = key_register_prefix("emCX4-");
	struct map *m = key_alloc();

	key_add(m, "C-Chr-X", cx_cmd);
	key_add(m, "emCX-Chr-4", cx4_cmd);
	key_add(m, "ESC", &emacs_meta);

	for (i = 0; i < ARRAY_SIZE(move_commands); i++) {
		struct move_command *mc = &move_commands[i];
		key_add(m, mc->k1, &mc->cmd);
		if (mc->k2)
			key_add(m, mc->k2, &mc->cmd);
		if (mc->k3)
			key_add(m, mc->k3, &mc->cmd);
	}

	for (i = 0; i < ARRAY_SIZE(str_commands); i++) {
		struct str_command *sc = &str_commands[i];
		key_add(m, sc->k, &sc->cmd);
	}

	key_add_range(m, "Chr- ", "Chr-~", &emacs_insert);
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

	key_add_range(m, "M-Chr-0", "M-Chr-9", &emacs_num);
	emacs_map = m;
}

DEF_LOOKUP_CMD(mode_emacs, emacs_map);

void emacs_search_init(struct editor *ed);
void edlib_init(struct editor *ed)
{
	if (emacs_map == NULL)
		emacs_init();
	key_add(ed->commands, "mode-emacs", &mode_emacs.c);
	key_add(ed->commands, "emacs:file-complete", &emacs_file_complete);
	key_add(ed->commands, "emacs:doc-complete", &emacs_doc_complete);
	emacs_search_init(ed);
}
