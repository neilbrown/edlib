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
#include <ctype.h>

#include "core.h"

REDEF_CMD(emacs_move);
REDEF_CMD(emacs_delete);
REDEF_CMD(emacs_case);

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

	{CMD(emacs_case), "LMove-Word", 1,
	 "M-Chr-l", NULL, NULL},
	{CMD(emacs_case), "UMove-Word", 1,
	 "M-Chr-u", NULL, NULL},
	{CMD(emacs_case), "CMove-Word", 1,
	 "M-Chr-c", NULL, NULL},
	{CMD(emacs_case), "TMove-Char", 1,
	 "M-Chr-`", NULL, NULL},
};

REDEF_CMD(emacs_move)
{
	struct move_command *mv = container_of(ci->comm, struct move_command, cmd);
	struct pane *cursor_pane = ci->focus;
	int old_x = -1;
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
		int y;
		if (mv->direction == 1)
			y = 0;
		else
			y = cursor_pane->h - 1;
		call_xy7("Mouse-event", cursor_pane, 1, 0, "Move-CursorXY", NULL,
			 old_x, y, ci->mark, NULL);
		if (mv->direction == 1)
			ok = mark_ordered_not_same_pane(cursor_pane, old_point, ci->mark);
		else
			ok = mark_ordered_not_same_pane(cursor_pane, ci->mark, old_point);
		if (!ok) {
			/* Try other end of pane */
			if (mv->direction != 1)
				y = 0;
			else
				y = cursor_pane->h - 1;
			call_xy7("Mouse-event", cursor_pane, 1, 0, "Move-CursorXY",
				 NULL, old_x, y, ci->mark, NULL);
		}
		mark_free(old_point);
	}

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

REDEF_CMD(emacs_case)
{
	struct move_command *mv = container_of(ci->comm, struct move_command, cmd);
	int ret = 0;
	struct mark *start = NULL;
	int cnt = mv->direction * RPT_NUM(ci);
	int dir;

	if (cnt == 0)
		return 1;
	if (cnt > 0) {
		dir = 1;
	} else {
		dir = -1;
		cnt = -cnt;
		start = mark_dup(ci->mark, 1);
	}

	while (cnt > 0) {
		struct mark *m = mark_dup(ci->mark, 1);

		ret = call3(mv->type+1, ci->focus, dir, ci->mark);
		if (ret <= 0 || mark_same_pane(ci->focus, ci->mark, m, NULL))
			/* Hit end of file */
			cnt = 1;
		else {
			char *str = doc_getstr(ci->focus, ci->mark, m);
			char *c;
			int changed = 0;
			int found = 0;

			for (c = str; *c; c += 1) {
				char type = mv->type[0];
				if (type == 'C') {
					type = found ? 'L' : 'U';
					if (isalpha(*c))
						found = 1;
				}
				switch(type) {
				default: /* silence compiler */
				case 'U': /* Uppercase */
					if (islower(*c)) {
						changed = 1;
						*c = toupper(*c);
					}
					break;
				case 'L': /* Lowercase */
					if (isupper(*c)) {
						changed = 1;
						*c = tolower(*c);
					}
					break;
				case 'T': /* Toggle */
					if (isupper(*c)) {
						changed = 1;
						*c = tolower(*c);
					} else if (islower(*c)) {
						changed = 1;
						*c = toupper(*c);
					}
					break;
				}
			}
			if (changed) {
				ret = call5("Replace", ci->focus, 1, m, str, ci->extra);
				if (dir < 0)
					call3(mv->type+1, ci->focus, dir, ci->mark);
			}
			free(str);
			pane_set_extra(ci->focus, 1);
		}
		mark_free(m);
		cnt -= 1;
	}
	/* When moving forward, move point.  When backward, leave point alone */
	if (start) {
		mark_to_mark(ci->mark, start);
		mark_free(start);
	}
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

	return call5(sc->type, ci->focus, ci->numeric, ci->mark, NULL, ci->extra);
}

REDEF_CMD(emacs_simple_neg)
{
	struct simple_command *sc = container_of(ci->comm, struct simple_command, cmd);

	return call5(sc->type, ci->focus, -RPT_NUM(ci), ci->mark, NULL, ci->extra);
}

DEF_CMD(emacs_exit)
{
	struct pane *p = ci->home;

	if (ci->numeric == NO_NUMERIC) {
		struct pane *p = call_pane7("PopupTile", ci->focus, 0, NULL, 0,
					    "DM", NULL);
		attr_set_str(&p->attrs, "done-key", "event:deactivate", -1);
		return call3("docs:show-modified", p, 0, 0);
	} else
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

REDEF_CMD(emacs_file_complete);
REDEF_CMD(emacs_doc_complete);

DEF_CMD(find_complete)
{
	char *type = ci->home->data;
	if (strcmp(type, "cmd") == 0)
		return 0;
	if (strcmp(type, "file") == 0)
		return emacs_file_complete_func(ci);
	else
		return emacs_doc_complete_func(ci);
}

DEF_CMD(find_done)
{
	int ret;
	char *str = doc_getstr(ci->focus, NULL, NULL);

	ret = call5("popup:close", ci->focus, 0, NULL, str, 0);
	free(str);
	return ret;
}

static struct
map *fh_map;
static void findmap_init(void)
{
	fh_map = key_alloc();
	key_add(fh_map, "Tab", &find_complete);
	key_add(fh_map, "Return", &find_done);
}

DEF_LOOKUP_CMD(find_handle, fh_map);

DEF_CMD(emacs_findfile)
{
	int fd;
	struct pane *p, *par;

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
		p = call_pane7("PopupTile", ci->focus, 0, NULL, 0, "D2", path);
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

		pane_register(pane_final_child(p), 0, &find_handle.c, "file", NULL);
		return 1;
	}

	if (strcmp(ci->key, "File Found Other Window") == 0)
		par = call_pane("OtherPane", ci->focus, 0, NULL, 0);
	else
		par = call_pane("ThisPane", ci->focus, 0, NULL, 1);

	if (!par)
		return -1;

	fd = open(ci->str, O_RDONLY);
	if (fd >= 0) {
		p = doc_open(par, fd, ci->str);
		close(fd);
	} else
		p = doc_from_text(par, ci->str, "File not found\n");
	if (p)
		doc_attach_view(par, p, NULL);
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

REDEF_CMD(emacs_file_complete)
{
	/* Extract a directory name and a basename from the document.
	 * Find a document for the directory and attach as a completing
	 * popup menu
	 */
	char *str = doc_getstr(ci->focus, NULL, NULL);
	char *d, *b, *c;
	int fd;
	struct pane *par, *pop, *docp;
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
	pop = call_pane7("PopupTile", ci->focus, 0, NULL, 0, "DM1r", NULL);
	if (!pop)
		return -1;
	par = doc_attach_view(pop, docp, NULL);

	attr_set_str(&par->attrs, "line-format", "%+name%suffix", -1);
	attr_set_str(&par->attrs, "heading", "", -1);
	attr_set_str(&par->attrs, "done-key", "Replace", -1);
	render_attach("complete", par);
	cr.c = save_str;
	cr.s = NULL;
	ret = call_comm("Complete:prefix", pane_final_child(par), 0, NULL,
			b, 0, &cr.c);
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

	if (strncmp(ci->key, "Doc Found", 9) != 0) {

		p = call_pane7("PopupTile", ci->focus, 0, NULL, 0, "D2", "");
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

		pane_register(pane_final_child(p), 0, &find_handle.c, "doc", NULL);
		return 1;
	}

	p = call_pane7("docs:byname", ci->focus, 0, NULL, 0, ci->str, NULL);
	if (!p)
		return -1;

	if (strcmp(ci->key, "Doc Found Other Window") == 0)
		par = call_pane("OtherPane", ci->focus, 0, NULL, 0);
	else
		par = call_pane("ThisPane", ci->focus, 0, NULL, 1);
	if (!p)
		return -1;

	p = doc_attach_view(par, p, NULL);
	return !!p;
}

REDEF_CMD(emacs_doc_complete)
{
	/* Extract a document from the document.
	 * Attach the 'docs' document as a completing popup menu
	 */
	char *str = doc_getstr(ci->focus, NULL, NULL);
	struct pane *par, *pop, *docs;
	struct call_return cr;
	int ret;

	pop = call_pane7("PopupTile", ci->focus, 0, NULL, 0,
			 "DM1r", NULL);
	if (!pop)
		return -1;
	docs = call_pane7("docs:byname", ci->focus, 0, NULL,
			  0, NULL, NULL);
	par = doc_attach_view(pop, docs, NULL);

	attr_set_str(&par->attrs, "line-format", "%+name", -1);
	attr_set_str(&par->attrs, "heading", "", -1);
	attr_set_str(&par->attrs, "done-key", "Replace", -1);
	render_attach("complete", par);
	cr.c = save_str;
	cr.s = NULL;
	ret = call_comm("Complete:prefix", pane_final_child(par), 0, NULL,
			str, 0, &cr.c);
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

	docs = call_pane7("docs:byname", ci->focus, 0, NULL, 0, "*Documents*", NULL);
	if (!docs)
		return -1;
	par = call_pane("ThisPane", ci->focus, 0, NULL, 1);
	if (!par)
		return -1;

	p = doc_attach_view(par, docs, NULL);
	return !!p;
}

DEF_CMD(emacs_shell)
{
	char *name = "*Shell Command Output*";
	struct pane *p, *doc, *par;
	if (strcmp(ci->key, "Shell Command") != 0) {
		p = call_pane7("PopupTile", ci->focus, 0, NULL, 0,
			       "D2", "");
		if (!p)
			return 0;
		attr_set_str(&p->attrs, "prefix", "Shell command: ", -1);
		attr_set_str(&p->attrs, "done-key", "Shell Command", -1);
		call5("doc:set-name", p, 0, NULL, "Shell Command", 0);
		p = call_pane7("attach-history", pane_final_child(p), 0, NULL, 0,
			       "*Shell History*", "popup:close");
		pane_register(pane_final_child(p), 0, &find_handle.c, "cmd", NULL);
		return 1;
	}
	par = call_pane("OtherPane", ci->focus, 0, NULL, 0);
	if (!par)
		return -1;
	/* Find or create "*Shell Command Output*" */
	doc = call_pane7("docs:byname", ci->focus, 0, NULL, 0, name, NULL);
	if (!doc)
		doc = doc_from_text(par, name, "");
	p = doc_attach(doc, doc);
	call_pane7("attach-shellcmd", p, 0, NULL, 0, ci->str, NULL);
	doc_attach_view(par, doc, NULL);
	return 1;
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
	return call3("doc:destroy", ci->focus, 0, 0);
}

DEF_CMD(emacs_save_all)
{
	if (ci->numeric == NO_NUMERIC) {
		struct pane *p = call_pane7("PopupTile", ci->focus, 0, NULL, 0,
					    "DM", NULL);
		return call3("docs:show-modified", p, 0, 0);
	} else
		return call3("docs:save-all", ci->focus, 0, 0);
}

DEF_CMD(emacs_search)
{
	struct mark *m;

	if (strcmp(ci->key, "Search String") != 0) {
		struct pane *p = call_pane7("PopupTile", ci->focus, 0, NULL,
					    0, "TR2", "");

		if (!p)
			return 0;

		attr_set_str(&p->attrs, "prefix", "Search: ", -1);
		attr_set_str(&p->attrs, "done-key", "Search String", -1);

		call5("doc:set-name", p, 0, NULL, "Search", 0);

		p = pane_final_child(p);
		call_pane("attach-emacs-search", p, 0, NULL, 0);
		return 1;
	}

	if (!ci->str || !ci->str[0])
		return -1;

	m = mark_at_point(ci->focus, NULL, MARK_UNGROUPED);

	call7("global-set-attr", ci->focus, 0, NULL, "Search String",
	      0, ci->str, NULL);

	if (call5("text-search", ci->focus, 0, m, ci->str, 0) > 1)
		call3("Move-to", ci->focus, 0, m);

	mark_free(m);
	return 1;
}

DEF_CMD(emacs_bury)
{
	/* Display something else in this tile. */
	struct pane *tile, *c;
	tile = call_pane("ThisPane", ci->focus, 0, NULL, 0);
	if (!tile)
		return 1;
	call5("doc:revisit", ci->focus, -1, NULL, NULL, 0);
	c = pane_child(tile);
	if (c)
		pane_close(c);
	c = call_pane("docs:choose", tile, 0, NULL, 0);
	if (c)
		doc_attach_view(tile, c, NULL);
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

	key_add(m, "emCX-Chr-s", &emacs_save_all);

	key_add(m, "C-Chr-S", &emacs_search);
	key_add(m, "Search String", &emacs_search);

	key_add(m, "emCX-C-Chr-C", &emacs_exit);

	key_add(m, "M-Chr-!", &emacs_shell);
	key_add(m, "Shell Command", &emacs_shell);

	key_add(m, "M-Chr-B", &emacs_bury);

	key_add_range(m, "M-Chr-0", "M-Chr-9", &emacs_num);
	emacs_map = m;
}

DEF_LOOKUP_CMD(mode_emacs, emacs_map);

DEF_CMD(attach_mode_emacs)
{
	return call_comm("global-set-keymap", ci->focus, 0, NULL, NULL, 0,
			 &mode_emacs.c);
}

void emacs_search_init(struct pane *ed);
void edlib_init(struct pane *ed)
{
	if (emacs_map == NULL)
		emacs_init();
	if (fh_map == NULL)
		findmap_init();
	call_comm("global-set-command", ed, 0, NULL, "attach-mode-emacs",
		  0, &attach_mode_emacs);
	emacs_search_init(ed);
}
