/*
 * Copyright Neil Brown ©2015-2023 <neil@brown.name>
 * May be distributed under terms of GPLv2 - see file:COPYING
 *
 * Define some keystrokes to create an editor with an
 * "emacs" feel.
 *
 */
#define _GNU_SOURCE /*  for asprintf */
#include <unistd.h>
#include <stdlib.h>
#include <wchar.h>
#include <fcntl.h>
#include <string.h>
#include <ctype.h>
#include <wctype.h>

#include <stdio.h>

#define PANE_DATA_PTR_TYPE char *
#include "core.h"
#include "core-pane.h"

static const char * safe file_normalize(struct pane *p safe, const char *path,
					const char *initial_path);

/* num2 is used to track if successive commands are related.
 * Only low 16 bits identify command, other bits are free.
 */
enum {
	N2_zero = 0,
	N2_undo_insert,	/* adjacent commands form a single undo set */
	N2_undo_delete,	/* adjacent commands form a single undo set */
	N2_undo_change,	/* adjacent commands form a single undo set */
	N2_recentre,	/* repeated recentre goes to different places */
	N2_yank,	/* repeated yank-pop takes different result */
	N2_match,	/* repeated ` after Cx` repeats the search */
	N2_undo,	/* Last command was 'undo' too */
	N2_close_others,/* Last command was close-other, just a 1 can repeat */
	N2_runmacro,	/* Last command was CX-e, just an 'e' can repeat */
	N2_shift,	/* Last command was CX-< or CX-> */
	N2_growx,	/* Last command was CX-{ or CX-} */
	N2_uniquote,	/* Last command was :C-q inserting a unicode from name */
};
static inline int N2(const struct cmd_info *ci safe)
{
	return ci->num2 & 0xffff;
}

static inline int N2a(const struct cmd_info *ci safe)
{
	return ci->num2 >> 16;
}

REDEF_CMD(emacs_move);
REDEF_CMD(emacs_delete);
REDEF_CMD(emacs_kill);
REDEF_CMD(emacs_case);
REDEF_CMD(emacs_swap);

static struct move_command {
	struct command	cmd;
	char		*type safe;
	int		direction, extra;
	char		*k1 safe, *k2, *k3;
} move_commands[] = {
	{CMD(emacs_move), "Move-Char", 1, 0,
	 "K:C-F", "K:Right", NULL},
	{CMD(emacs_move), "Move-Char", -1, 0,
	 "K:C-B", "K:Left", NULL},
	{CMD(emacs_move), "doc:word", 1, 0,
	 "K:A-f", "K:A:Right", NULL},
	{CMD(emacs_move), "doc:word", -1, 0,
	 "K:A-b", "K:A:Left", NULL},
	{CMD(emacs_move), "doc:expr", 1, 0,
	 "K:A:C-F", NULL, NULL},
	{CMD(emacs_move), "doc:expr", -1, 0,
	 "K:A:C-B", NULL, NULL},
	{CMD(emacs_move), "doc:expr", -1, 1,
	 "K:A:C-U", NULL, NULL},
	{CMD(emacs_move), "doc:expr", 1, 1,
	 "K:A:C-D", NULL, NULL},
	{CMD(emacs_move), "doc:WORD", 1, 0,
	 "K:A-F", NULL, NULL},
	{CMD(emacs_move), "doc:WORD", -1, 0,
	 "K:A-B", NULL, NULL},
	{CMD(emacs_move), "doc:EOL", 1, 0,
	 "K:C-E", "K:End", NULL},
	{CMD(emacs_move), "doc:EOL", -1, 0,
	 "K:C-A", "K:Home", NULL},
	{CMD(emacs_move), "Move-Line", -1, 0,
	 "K:C-P", "K:Up", NULL},
	{CMD(emacs_move), "Move-Line", 1, 0,
	 "K:C-N", "K:Down", NULL},
	{CMD(emacs_move), "doc:file", 1, 0,
	 "K:A->", "K:S:End", NULL},
	{CMD(emacs_move), "doc:file", -1, 0,
	 "K:A-<", "K:S:Home", NULL},
	{CMD(emacs_move), "Move-View", 900, 0,
	 "K:Next", "K:C-V", "emacs-move-large-other"},
	{CMD(emacs_move), "Move-View", -900, 0,
	 "K:Prior", "K:A-v", NULL},

	{CMD(emacs_move), "doc:paragraph", -1, 0,
	 "K:A:C-A", NULL, NULL},
	{CMD(emacs_move), "doc:paragraph", 1, 0,
	 "K:A:C-E", NULL, NULL},

	{CMD(emacs_delete), "Move-Char", 1, 0,
	 "K:C-D", "K:Del", "del"},
	{CMD(emacs_delete), "Move-Char", -1, 0,
	 "K:C-H", "K:Backspace", "K:Delete"},
	{CMD(emacs_delete), "doc:word", 1, 0,
	 "K:A-d", NULL, NULL},
	{CMD(emacs_delete), "doc:word", -1, 0,
	 "K:A:C-H", "K:A:Backspace", NULL},
	{CMD(emacs_kill), "doc:EOL", 1, 0,
	 "K:C-K", NULL, NULL},
	{CMD(emacs_kill), "doc:expr", 1, 0,
	 "K:A:C-K", NULL, NULL},

	{CMD(emacs_case), "Ldoc:word", 1, 0,
	 "K:A-l", "K:A-L", NULL},
	{CMD(emacs_case), "Udoc:word", 1, 0,
	 "K:A-u", "K:A-U", NULL},
	{CMD(emacs_case), "Cdoc:word", 1, 0,
	 "K:A-c", "K:A-C", NULL},
	{CMD(emacs_case), "TMove-Char", 1, 0,
	 "K:A-`", NULL, NULL},

	{CMD(emacs_swap), "Move-Char", 1, 0,
	 "K:C-T", NULL, NULL},
	{CMD(emacs_swap), "doc:word", 1, 0,
	 "K:A-t", NULL, NULL},
	{CMD(emacs_swap), "doc:WORD", 1, 0,
	 "K:A-T", NULL, NULL},
};

REDEF_CMD(emacs_move)
{
	struct move_command *mv = container_of(ci->comm, struct move_command, cmd);
	struct pane *cursor_pane = ci->focus;
	int ret = 0;
	struct mark *mk;

	if (!ci->mark)
		return Enoarg;

	/* if doc:file, leave inactive mark behind */
	if (strcmp(mv->type, "doc:file") == 0) {
		mk = call_ret(mark2, "doc:point", ci->focus);
		if (mk)
			/* Don't change selection:active */
			mark_to_mark(mk, ci->mark);
		else {
			call("Move-to", ci->focus, 1, ci->mark);
			mk = call_ret(mark2, "doc:point", ci->focus);
		}
	}

	ret = call(mv->type, ci->focus, mv->direction * RPT_NUM(ci), ci->mark,
		   NULL, mv->extra);
	if (ret < 0)
		return ret;

	if (strcmp(mv->type, "Move-View") == 0)
		attr_set_int(&cursor_pane->attrs, "emacs-repoint",
			     mv->direction*2);

	mk = call_ret(mark2, "doc:point", ci->focus);
	/* Discard a transient selection */
	call("selection:clear", ci->focus, 2, mk);

	return 1;
}

REDEF_CMD(emacs_delete)
{
	struct move_command *mv = container_of(ci->comm, struct move_command, cmd);
	int ret;
	struct mark *m, *mk;

	if (!ci->mark)
		return Enoarg;

	mk = call_ret(mark2, "doc:point", ci->focus);
	/* If selection is replacable, clear it and use mk */
	if (call("selection:clear", ci->focus, 3, mk) == Efalse) {
		/* else clear any transient selection */
		call("selection:clear", ci->focus, 2, mk);
		mk = NULL;
	}

	m = mark_dup(ci->mark);

	ret = call(mv->type, ci->focus, mv->direction * RPT_NUM(ci), m);

	if (ret < 0) {
		mark_free(m);
		return ret;
	}
	if (mk)
		call("Replace", ci->focus, 1, mk, NULL, 0, mk);

	ret = call("Replace", ci->focus, 1, m, NULL, N2(ci) == N2_undo_delete);
	mark_free(m);
	call("Mode:set-num2", ci->focus, N2_undo_delete);

	return 1;
}

REDEF_CMD(emacs_kill)
{
	/* Like delete, but copy to copy-buffer */
	struct move_command *mv = container_of(ci->comm, struct move_command, cmd);
	int ret;
	struct mark *m;
	char *str;

	if (!ci->mark)
		return Enoarg;

	m = mark_dup(ci->mark);

	if (strcmp(mv->type, "doc:EOL") == 0 &&
	    mv->direction == 1 && RPT_NUM(ci) == 1 &&
	    is_eol(doc_following(ci->focus, m)))
		ret = call("Move-Char", ci->focus, mv->direction * RPT_NUM(ci), m);
	else
		ret = call(mv->type, ci->focus, mv->direction * RPT_NUM(ci), m);

	if (ret < 0) {
		mark_free(m);
		return ret;
	}
	/* Remove any selection so it cannot be claimed and so replace this copy */
	call("selection:claim", ci->focus);
	call("selection:discard", ci->focus);

	str = call_ret(strsave, "doc:get-str", ci->focus, 0, NULL, NULL, 0, m);
	if (str && *str)
		call("copy:save", ci->focus, N2(ci) == N2_undo_delete, NULL, str);
	ret = call("Replace", ci->focus, 1, m, NULL, N2(ci) == N2_undo_delete);
	mark_free(m);
	call("Mode:set-num2", ci->focus, N2_undo_delete);

	return 1;
}

REDEF_CMD(emacs_case)
{
	struct move_command *mv = container_of(ci->comm, struct move_command, cmd);
	int ret;
	struct mark *start = NULL;
	int cnt = mv->direction * RPT_NUM(ci);
	int dir;

	if (!ci->mark)
		return Enoarg;

	if (cnt == 0)
		return 1;
	if (cnt > 0) {
		dir = 1;
	} else {
		dir = -1;
		cnt = -cnt;
		start = mark_dup(ci->mark);
	}

	while (cnt > 0) {
		struct mark *m = mark_dup(ci->mark);

		ret = call(mv->type+1, ci->focus, dir, ci->mark);
		if (ret <= 0 || mark_same(ci->mark, m))
			/* Hit end of file */
			cnt = 1;
		else {
			char *str = call_ret(str, "doc:get-str", ci->focus,
					     0, ci->mark, NULL,
					     0, m);
			char *c;
			int changed = 0;
			int found = 0;

			for (c = str; c && *c; c += 1) {
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
				ret = call("Replace", ci->focus, 1, m, str,
					   N2(ci) == N2_undo_change);
				if (dir < 0)
					call(mv->type+1, ci->focus, dir, ci->mark);
			}
			free(str);
			if (changed || N2(ci) == N2_undo_change)
				call("Mode:set-num2", ci->focus, N2_undo_change);
		}
		mark_free(m);
		cnt -= 1;
	}
	/* When moving forward, move point.  When backward, leave point alone */
	if (start) {
		mark_to_mark(ci->mark, start);
		mark_free(start);
	}
	return 1;
}

REDEF_CMD(emacs_swap)
{
	/* collect the object behind point and insert it after the object
	 * after point.  With a +ve repeat count, insert after n objects.
	 * With -ve repeast, collect object after and insert behind n
	 * previous objects.  Object is determined by mv->type.
	 */
	struct move_command *mv = container_of(ci->comm, struct move_command, cmd);
	struct mark *start = NULL;
	int cnt = mv->direction * RPT_NUM(ci);
	int dir;

	if (!ci->mark)
		return Enoarg;

	if (cnt == 0)
		return 1;
	if (cnt > 0) {
		dir = 1;
	} else {
		dir = -1;
		cnt = -cnt;
		start = mark_dup(ci->mark);
	}

	while (cnt > 0) {
		struct mark *as, *ae, *bs, *be;
		char *astr, *bstr;

		call(mv->type, ci->focus, -dir, ci->mark);
		as = mark_dup(ci->mark);
		call(mv->type, ci->focus, dir, ci->mark);
		ae = mark_dup(ci->mark);
		/* as to ae is the object to be moved. */

		call(mv->type, ci->focus, dir, ci->mark);
		be = mark_dup(ci->mark);
		call(mv->type, ci->focus, -dir, ci->mark);
		bs = mark_dup(ci->mark);
		/* bs to be is the object to be swapped with.
		 * bs must be after ae in the direction
		 */
		if (mark_same(as, ae) ||
		    mark_same(bs, be) ||
		    bs->seq - ae->seq * dir < 0) {
			mark_to_mark(ci->mark, ae);
			mark_free(as);
			mark_free(ae);
			mark_free(bs);
			mark_free(be);
			break;
		}
		astr = call_ret(str, "doc:get-str", ci->focus,
				0, as, NULL,
				0, ae);
		bstr = call_ret(str, "doc:get-str", ci->focus,
				0, bs, NULL,
				0, be);
		mark_to_mark(ci->mark, ae);
		call("Replace", ci->focus, 1, as, bstr);
		mark_to_mark(ci->mark, be);
		call("Replace", ci->focus, 1, bs, astr, 1);
		if (dir < 0)
			call(mv->type, ci->focus, dir, ci->mark);
		free(astr);
		free(bstr);
		mark_free(as);
		mark_free(ae);
		mark_free(bs);
		mark_free(be);
		cnt -= 1;
	}
	/* When moving forward, move point.  When backward, leave point alone */
	if (start) {
		mark_to_mark(ci->mark, start);
		mark_free(start);
	}
	return 1;
}

DEF_CMD(emacs_move_view_other)
{
	/* If there is an 'other' pane', Send "K:Next" there */
	struct pane *p;

	/* '512' means 'fail if no other pane' */
	p = call_ret(pane, "OtherPane", ci->focus, 512);
	if (!p)
		return 1;
	call("Mode:set-num", p, ci->num);
	call("Keystroke", p, 0, NULL, "emacs-move-large-other");
	return 1;
}

DEF_CMD(emacs_recenter)
{
	int step = 0;
	if (ci->num == NO_NUMERIC && N2(ci) == N2_recentre) {
		/* Repeated command - go to top, or bottom, or middle in order */
		switch (N2a(ci)) {
		default:
		case 0: /* was center, go to top */
			call("Move-View-Line", ci->focus, 1, ci->mark);
			step = 1;
			break;
		case 1: /* was top, go to bottom */
			call("Move-View-Line", ci->focus, -1, ci->mark);
			step = 2;
			break;
		case 2: /* was bottom, go to middle */
			call("Move-View-Line", ci->focus, 0, ci->mark);
			step = 0;
			break;
		}
	} else if (ci->num == -NO_NUMERIC) {
		/* Move point to bottom */
		call("Move-View-Line", ci->focus, -1, ci->mark);
		step = 2;
	} else if (ci->num != NO_NUMERIC) {
		/* Move point to display line N */
		call("Move-View-Line", ci->focus, ci->num, ci->mark);
	} else {
		/* Move point to middle and refresh */
		call("Move-View-Line", ci->focus, 0, ci->mark);
		call("Window:refresh", ci->focus);
	}
	call("Mode:set-num2", ci->focus, N2_recentre | (step << 16));
	return 1;
}

REDEF_CMD(emacs_simple);
REDEF_CMD(emacs_simple_num);
REDEF_CMD(emacs_simple_str);
static struct simple_command {
	struct command	cmd;
	char		*type safe;
	char		*k safe;
} simple_commands[] = {
	{CMD(emacs_simple), "Tile:next", "K:CX-o"},
	{CMD(emacs_simple), "Tile:prev", "K:CX-O"},
	{CMD(emacs_simple), "Tile:y+", "K:CX-^"},
	{CMD(emacs_simple), "Tile:split-y", "K:CX-2"},
	{CMD(emacs_simple), "Tile:split-x", "K:CX-3"},
	{CMD(emacs_simple), "Tile:close", "K:CX-0"},
	{CMD(emacs_simple), "Tile:bury", "K:A-B"},
	{CMD(emacs_simple), "Window:new", "K:CX5-2"},
	{CMD(emacs_simple), "Window:close", "K:CX5-0"},
	{CMD(emacs_simple), "lib-server:done", "K:CX-#"},
	{CMD(emacs_simple), "mode-swap-mark", "K:CX:C-X"},
	{CMD(emacs_simple), "Abort", "K:C-G"},
	{CMD(emacs_simple), "Cancel", "K:ESC"},
	{CMD(emacs_simple), "NOP", "K:A-G"},
	{CMD(emacs_simple), "NOP", "K:CX:C-G"},
	{CMD(emacs_simple), "NOP", "K:CX4:C-G"},
	{CMD(emacs_simple), "doc:save-file", "K:CX:C-S"},
	{CMD(emacs_simple), "Commit", "K:CC:C-C"},
	/* one day, this will be "find definition", now it is same as "find any" */
	{CMD(emacs_simple_num), "interactive-cmd-git-grep", "K:CX:A-."},
	{CMD(emacs_simple_str), "interactive-cmd-git-grep", "K:A-."},
	{CMD(emacs_simple), "interactive-cmd-merge-mode", "K:A-m"},
	{CMD(emacs_simple_str), "interactive-cmd-calc-replace", "K:A-#"},
};

REDEF_CMD(emacs_simple)
{
	struct simple_command *sc = container_of(ci->comm, struct simple_command, cmd);

	if (!ci->mark)
		return Enoarg;

	return call(sc->type, ci->focus, ci->num, ci->mark);
}

REDEF_CMD(emacs_simple_num)
{
	struct simple_command *sc = container_of(ci->comm, struct simple_command, cmd);

	if (!ci->mark)
		return Enoarg;

	return call(sc->type, ci->focus, RPT_NUM(ci), ci->mark);
}

REDEF_CMD(emacs_simple_str)
{
	struct simple_command *sc = container_of(ci->comm, struct simple_command, cmd);
	struct mark *mk = call_ret(mark2, "doc:point", ci->focus);
	char *str = NULL;

	if (!ci->mark)
		return Enoarg;
	if (call("selection:clear", ci->focus, 0, mk) >= 1)
		str = call_ret(strsave, "doc:get-str", ci->focus, 0, NULL, NULL, 0, mk);

	return call(sc->type, ci->focus, ci->num, ci->mark, str, 0, mk);
}

DEF_CMD(emacs_close_others)
{
	if (strcmp(ci->key, "K-1") == 0 && N2(ci) != N2_close_others)
		return Efallthrough;

	if (call("Tile:close-others", ci->focus) <= 0)
		return Efalse;
	call("Mode:set-num2", ci->focus, N2_close_others);
	call("Message:modal", ci->focus, 0, NULL, "Type 1 to close more");
	return 1;
}

DEF_CB(cnt_disp)
{
	struct call_return *cr = container_of(ci->comm, struct call_return, c);

	cr->i += 1;
	return 1;
}

DEF_CMD(emacs_deactivate)
{
	/* close-all popup has closed, see if it aborted */
	if (ci->str)
		call("event:deactivate", ci->focus);
	return 1;
}

DEF_CMD(emacs_exit)
{
	struct call_return cr;

	cr.c = cnt_disp;
	cr.i = 0;
	if (ci->num == NO_NUMERIC) {
		struct pane *p;

		/* If this is not only display, then refuse to exit */
		call_comm("editor:notify:all-displays", ci->focus, &cr.c);
		if (cr.i > 1) {
			call("Message", ci->focus, 0, NULL,
			     "Cannot exit when there are multiple windows.");
			return 1;
		}

		p = call_ret(pane, "PopupTile", ci->focus, 0, NULL, "DM");
		if (!p) {
			/* Probably called from a pop-up.  Abort the popup
			 * and let the user try again.
			 */
			call("Abort", ci->focus);
			return Efail;
		}
		attr_set_str(&p->attrs, "done-key", "emacs:deactivate");
		call("docs:show-modified", p);
		return 1;
	} else
		call("event:deactivate", ci->focus);
	return 1;
}

DEF_CMD(emacs_quote_insert)
{
	int ret;
	char buf[2] = ".";
	const char *str;
	bool first = N2(ci) != N2_undo_insert;

	if (!ci->mark)
		return Enoarg;

	if (call("selection:clear", ci->focus, 3, ci->mark) >= 1) {
		call("Replace", ci->focus, 1, ci->mark, NULL, !first);
		first = False;
	} else
		call("selection:clear", ci->focus, 2, ci->mark);

	str = ksuffix(ci, "K:CQ-");
	if (!str[0]) {
		str = ksuffix(ci, "K:CQ:C-");
		if (str[0]) {
			buf[0] = str[0] & 0x1f;
			str = buf;
		} else
			str = "??";
	}
	ret = call("Replace", ci->focus, 1, ci->mark, str, !first);
	call("Mode:set-num2", ci->focus, N2_undo_insert);

	return ret < 0 ? ret : 1;
}

DEF_CMD(emacs_open_line)
{
	if (call("Interactive:insert", ci->focus, 0, ci->mark, "\n") > 0)
		call("Move-Char", ci->focus, -1, ci->mark);
	return 1;
}

DEF_CMD(emacs_undo)
{
	int ret;

	ret = call("doc:reundo", ci->focus, N2(ci) == N2_undo);
	call("Mode:set-num2", ci->focus, N2_undo);
	if (ret == Efalse)
		call("Message", ci->focus, 0, NULL,
		     "No further undo information");

	return 1;
}

REDEF_CMD(emacs_file_complete);
REDEF_CMD(emacs_doc_complete);
REDEF_CMD(emacs_cmd_complete);

DEF_CMD(find_complete)
{
	char *type = ci->home->data;

	if (strstarts(type, "file"))
		return emacs_file_complete_func(ci);
	if (strcmp(type, "shellcmd") == 0)
		return emacs_file_complete_func(ci);
	if (strcmp(type, "doc") == 0)
		return emacs_doc_complete_func(ci);
	if (strcmp(type, "cmd") == 0)
		return emacs_cmd_complete_func(ci);
	return Efallthrough;
}

DEF_CMD(find_done)
{
	int ret;
	char *type = ci->home->data;
	char *str = call_ret(strsave, "doc:get-str", ci->focus);
	const char *norm = NULL;
	struct stat stb;

	if (!str || !str[0])
		/* close with no string */
		ret = call("popup:close", ci->focus);
	if (strcmp(type, "doc") == 0 &&
	    call_ret(pane, "docs:byname", ci->focus, 0, NULL, str) == NULL) {
		call("Message:modal", ci->focus, 0, NULL, "Document not found");
		return 1;
	}
	if (strstarts(type, "file")) {
		char *sl;
		bool can_create = True;
		bool can_create_dir = True;

		norm = file_normalize(ci->focus, str,
				      attr_find(ci->home->attrs,
						"initial_path"));
		sl = strrchr(norm, '/');
		while (sl && sl > norm) {
			/* Need to check directories exist. */
			*sl = 0;
			stb.st_mode = 0;
			stat(norm, &stb);
			if ((stb.st_mode & S_IFMT) == S_IFDIR)
				break;
			if (stb.st_mode)
				can_create_dir = False;
			can_create = False;
			sl = strrchr(norm, '/');
		}
		if (sl)
			*sl = '/';
		if (!can_create_dir) {
			call("Message:modal", ci->focus, 0, NULL,
			     strconcat(ci->focus, norm, " is not a directory!!"));
			return 1;
		}
		if (!can_create) {
			/* Need to create directory first */
			if (strcmp(ci->key, "K:A:Enter") == 0) {
				char *m = "Cannot create directory: ";
				if (mkdir(norm, 0777) == 0) {
					m = "Created directory: ";
					attr_set_str(&ci->home->attrs,
						     "prev_dir", NULL);
					/* Trigger recalc on replace */
					call("doc:replace", ci->focus, 0, NULL, "");
				}
				call("Message:modal", ci->focus, 0, NULL,
				     strconcat(ci->focus, m, norm));
				return 1;
			}
			call("Message:modal", ci->focus, 0, NULL,
			     strconcat(ci->focus,
				       "Use Alt-Enter to create directory ", norm));
			return 1;
		}
	}
	if (strcmp(type, "file") == 0 && norm &&
	    strcmp(ci->key, "K:Enter") == 0 &&
	    stat(norm, &stb) != 0) {
		call("Message:modal", ci->focus, 0, NULL,
		     "File not found - use Alt-Enter to create");
		return 1;
	}
	if (strcmp(type, "file write") == 0 && norm &&
	    strcmp(ci->key, "K:Enter") == 0 &&
	    stat(norm, &stb) == 0) {
		call("Message:modal", ci->focus, 0, NULL,
		     "File exists - use Alt-Enter to overwrite");
		return 1;
	}
	ret = call("popup:close", ci->focus, 0, NULL, norm ?: str);
	return ret < 0 ? ret : 1;
}

struct find_helper {
	char *name;
	int want_prev;
	struct pane *ret;
	struct command c;
};

DEF_CB(find_helper)
{
	struct find_helper *h = container_of(ci->comm, struct find_helper, c);
	struct pane *p = ci->focus;
	char *name;

	if (!p)
		return 1;
	if (!h->name) {
		if (h->want_prev) {
			/* want the pane before nothing, so the last.
			 * So return this one and keep looking.
			 */
			h->ret = p;
			return 1;
		} else {
			/* Want the pane that is after nothing, so
			 * the first.  This one. All done.
			 */
			h->ret = p;
			return Efalse;
		}
	}
	name = pane_attr_get(ci->focus, "doc-name");
	if (!name)
		return 1;
	if (strcmp(name, h->name) == 0) {
		if (h->want_prev) {
			/* Want the previous one, which is
			 * already in ->ret
			 */
			return Efalse;
		} else {
			/* Want the next one, so clear name
			 * and keep going.
			 */
			h->name = NULL;
			return 1;
		}
	} else {
		if (h->want_prev) {
			/* This might be what I want - keep it in case */
			h->ret = p;
			return 1;
		} else {
			/* Don't want this - just keep going */
			return 1;
		}
	}
}

DEF_CMD(find_prevnext)
{
	/* Find the previous document lru or, which is "next" as we
	 * walk the list in mru order.
	 * When we find it, insert the name into ci->focus document
	 */
	char *type = ci->home->data;
	struct find_helper h;

	if (strcmp(type, "doc") != 0)
		return Efallthrough;
	h.name = attr_find(ci->home->attrs, "find-doc");
	h.ret = NULL;
	h.c = find_helper;
	h.want_prev = strcmp(ci->key, "K:A-n") == 0;

	call_comm("docs:byeach", ci->focus, &h.c);
	if (h.ret) {
		char *name = pane_attr_get(h.ret, "doc-name");
		struct mark *m, *m2;

		attr_set_str(&ci->home->attrs, "find-doc", name);
		m = mark_new(ci->focus);
		m2 = m ? mark_dup(m) : NULL;
		call("doc:file", ci->focus, -1, m);
		call("doc:file", ci->focus, 1, m2);
		call("Replace", ci->focus, 1, m, name, 0, m2);
		mark_free(m);
		mark_free(m2);
	}
	return 1;
}

DEF_CMD(find_attr)
{
	char *type = ci->home->data;

	if (!ci->str)
		return Enoarg;

	if (strcmp(type, "file") != 0)
		return Efallthrough;

	if (strcmp(ci->str, "start-of-line") == 0) {
		char *lens = attr_find(ci->home->attrs, "path_lengths");
		int dir_start = 0, dir_end = 0, nondir_end = 0,
			basename_start = 0;
		if (lens)
			sscanf(lens, "%d %d %d %d", &dir_start, &dir_end,
			       &nondir_end, &basename_start);

		if (dir_start > 0)
			comm_call(ci->comm2, "cb", ci->focus, dir_start,
				  ci->mark, "fg:grey+20,nobold,noinverse", 115);
		if (dir_end > dir_start)
			comm_call(ci->comm2, "cb", ci->focus, dir_end,
				  ci->mark, "fg:black,nobold,noinverse", 114);
		if (nondir_end > dir_end)
			comm_call(ci->comm2, "cb", ci->focus, nondir_end,
				  ci->mark, "fg:red-80,bold,inverse", 113);
		if (basename_start > nondir_end)
			comm_call(ci->comm2, "cb", ci->focus, basename_start,
				  ci->mark, "fg:magenta", 112);
		comm_call(ci->comm2, "cb", ci->focus, 10000, ci->mark,
			  "fg:black", 111);
	}
	return 1;
}

DEF_CMD(find_check_replace)
{
	char *str, *cp, *sl;
	char *type = ci->home->data;
	char *initial_path;
	char *prev_dir;
	struct stat stb;
	int ipl; // Initial Path Len
	int dir_start, dir_end, nondir_end, basename_start;
	char nbuf[4 * (10+1) + 1], *lens;

	if (strcmp(type, "file") != 0)
		return Efallthrough;

	home_call(ci->home->parent, ci->key, ci->focus,
		  ci->num, ci->mark, ci->str,
		  ci->num2, ci->mark2, ci->str2);

	/* The doc content can have 5 different sections that might
	 * be different colours.
	 *  - ignored prefix: grey - This inital_path followed by something
	 *          that looks like another path. "/" or "~/"
	 *  - True directories: black - directory part of the path that
	 *          exists and is a directory
	 *  - non-directory-in-path: red - directory part that exists but
	 *          is not a directory.  At most one component.
	 *  - non-existant name: magenta - directory path that doesn't exist.
	 *  - basename: black, whether it exists or not.
	 * These are found as:
	 *  - dir_start
	 *  - dir_end
	 *  - nondir_end
	 *  - basename_start
	 * These are all lengths from start of path.  They are all stored
	 * in a single attribute: "path_lengths".
	 */

	str = call_ret(str, "doc:get-str", ci->focus);
	if (!str)
		return 1;
	sl = strrchr(str, '/');
	if (!sl)
		sl = str;

	prev_dir = attr_find(ci->home->attrs, "prev_dir");
	if (prev_dir && strlen(prev_dir) == (size_t)(sl - str + 1) &&
	    strncmp(prev_dir, str, sl - str + 1) == 0)
		/* No change before last '/' */
		return 1;

	initial_path = attr_find(ci->home->attrs, "initial_path");

	if (!initial_path)
		return 1;
	ipl = strlen(initial_path);
	cp = str;
	if (strncmp(str, initial_path, ipl) == 0 &&
	    (str[ipl] == '/' || (str[ipl] == '~' &&
				(str[ipl+1] == '/' ||
				 str[ipl+1] == 0))))
		cp = str + ipl;

	dir_start = utf8_strnlen(str, cp - str);

	basename_start = utf8_strnlen(str, sl - str + 1);
	stb.st_mode = 0;
	if (sl < cp)
		sl = cp;
	while (sl > cp) {
		const char *path;
		*sl = 0;
		path = file_normalize(ci->focus, str, initial_path);
		stat(path, &stb);
		*sl = '/';
		if (stb.st_mode)
			break;
		sl -= 1;
		while (sl > cp && *sl != '/')
			sl -= 1;
	}
	nondir_end = utf8_strnlen(str, sl - str + 1);
	dir_end = nondir_end;
	if (stb.st_mode != 0 &&
	    (stb.st_mode & S_IFMT) != S_IFDIR) {
		/* There is a non-dir on the path */
		while (sl > cp && sl[-1] != '/')
			sl -= 1;
		/* This must actually be a dir */
		dir_end = utf8_strnlen(str, sl - str);
	}
	snprintf(nbuf, sizeof(nbuf), "%d %d %d %d",
		 dir_start, dir_end, nondir_end, basename_start);
	lens = attr_find(ci->home->attrs, "path_lengths");
	if (!lens || strcmp(lens, nbuf) != 0)
		attr_set_str(&ci->home->attrs, "path_lengths", nbuf);
	sl = strrchr(str, '/');
	if (sl) {
		sl[1] = 0;
		attr_set_str(&ci->home->attrs, "prev_dir", str);
	}
	return 1;
}

static struct map *fh_map;
DEF_LOOKUP_CMD(find_handle, fh_map);

static void findmap_init(void)
{
	if (fh_map)
		return;
	fh_map = key_alloc();
	key_add(fh_map, "K:Tab", &find_complete);
	key_add(fh_map, "K:Enter", &find_done);
	key_add(fh_map, "K:A:Enter", &find_done);
	key_add(fh_map, "K:A-p", &find_prevnext);
	key_add(fh_map, "K:A-n", &find_prevnext);
	key_add(fh_map, "map-attr", &find_attr);
	key_add(fh_map, "doc:replace", &find_check_replace);
}

static const char * safe file_normalize(struct pane *p safe,
					const char *path,
					const char *initial_path)
{
	int len = strlen(initial_path?:"");
	char *dir;

	if (!path)
		return ".";
	if (initial_path && strncmp(path, initial_path, len) == 0) {
		if (path[len] == '/' || (path[len] == '~' &&
					 (path[len+1] == '/' || path[len+1] == 0)))
			path = path + len;
	}
	if (path[0] == '/')
		return path;
	if (path[0] == '~' && (path[1] == '/' || path[1] == 0)) {
		char *home = getenv("HOME");
		if (!home)
			home = "/";
		if (home[strlen(home) - 1] == '/' && path[1] == '/')
			path += 1;
		return strconcat(p, home, path+1);
	}
	dir = pane_attr_get(p, "dirname");
	if (!dir)
		dir = "/";
	if (dir[strlen(dir)-1] == '/')
		return strconcat(p, dir, path);
	return strconcat(p, dir, "/", path);
}

DEF_CMD(emacs_findfile)
{
	int fd;
	struct pane *p, *par;
	const char *path;
	char buf[PATH_MAX];
	char *e;

	path = pane_attr_get(ci->focus, "dirname");
	if (path) {
		strcpy(buf, path);
		path = buf;
	}
	if (!path) {
		strcpy(buf, "/");
		path = buf;
	}
	e = buf + strlen(buf);
	if (e < buf + sizeof(buf)-1 && e > buf && e[-1] != '/') {
		*e++ = '/';
		*e++ = '\0';
	}

	if (ksuffix(ci, "File Found:")[0] == 0) {
		p = call_ret(pane, "PopupTile", ci->focus, 0, NULL, "D2",
			     0, NULL, path);
		if (!p)
			return Efail;

		if (ksuffix(ci, "K:CX44")[0]) {
			attr_set_str(&p->attrs, "prompt",
				     "Find File Popup");
			attr_set_str(&p->attrs, "done-key",
				     "File Found:Popup");
		} else if (ksuffix(ci, "K:CX4")[0]) {
			attr_set_str(&p->attrs, "prompt",
				     "Find File Other Window");
			attr_set_str(&p->attrs, "done-key",
				     "File Found:Other Window");
		} else {
			attr_set_str(&p->attrs, "prompt", "Find File");
			attr_set_str(&p->attrs, "done-key",
				     "File Found:This Window");
		}
		call("doc:set-name", p, 0, NULL, "Find File");

		p = pane_register(p, 0, &find_handle.c, "file");
		if (!p)
			return Efail;
		attr_set_str(&p->attrs, "initial_path", path);
		call("attach-history", p, 0, NULL, "*File History*");
		return 1;
	}
	if (!ci->str)
		/* Aborted */
		return Efail;

	path = file_normalize(ci->focus, ci->str, path);

	if (!path)
		return Efail;

	fd = open(path, O_RDONLY);
	if (fd >= 0) {
		p = call_ret(pane, "doc:open", ci->focus, fd, NULL, path);
		close(fd);
	} else
		/* '4' says 'allow create' */
		p = call_ret(pane, "doc:open", ci->focus, -1, NULL, path, 4);
	if (!p) {
		char *m = NULL;
		asprintf(&m, "Failed to open file: %s", path);
		call("Message", ci->focus, 0, NULL, m);
		free(m);
		return Efail;
	}
	if (strcmp(ci->key, "File Found:Other Window") == 0) {
		struct pane *this = call_ret(pane, "ThisPane", ci->focus);
		par = home_call_ret(pane, ci->focus, "DocPane", p);
		if (par && par != this) {
			pane_take_focus(par);
			return 1;
		}
		par = call_ret(pane, "OtherPane", ci->focus);
	} else if (strcmp(ci->key, "File Found:Popup") == 0) {
		par = call_ret(pane, "PopupTile", ci->focus, 0, NULL, "MD3tsa");
	} else
		par = call_ret(pane, "ThisPane", ci->focus);

	if (!par) {
		pane_close(p);
		return Efail;
	}

	p = home_call_ret(pane, p, "doc:attach-view", par, 1);
	if (p)
		pane_take_focus(p);

	return p ? 1 : Efail;
}

DEF_CMD(emacs_writefile)
{
	/* Request to write to a different file */
	struct pane *p;
	const char *path;
	char buf[PATH_MAX];
	char *e;

	if (call("doc:write-file", ci->focus, NO_NUMERIC) >= 0) {
		/* It should have been an error ! */
		const char *doc = pane_attr_get(ci->focus, "doc-name");
		if (doc)
			call("Message", ci->focus, 0, NULL,
			     strconcat(ci->focus, "Document ", doc,
				       " cannot be written"));
		else
			call("Message", ci->focus, 0, NULL,
			     "This document cannot be written");
		return Efail;
	}
	path = pane_attr_get(ci->focus, "dirname");
	if (path) {
		strcpy(buf, path);
		path = buf;
	}
	if (!path) {
		strcpy(buf, "/");
		path = buf;
	}
	e = buf + strlen(buf);
	if (e < buf + sizeof(buf)-1 && e > buf && e[-1] != '/') {
		*e++ = '/';
		*e++ = '\0';
	}

	p = call_ret(pane, "PopupTile", ci->focus, 0, NULL, "D2",
		     0, NULL, path);
	if (!p)
		return Efail;
	attr_set_str(&p->attrs, "prompt", "Write File");
	attr_set_str(&p->attrs, "done-key",
		     strconcat(p, "emacs:write_file:", path));
	call("doc:set-name", p, 0, NULL, "Write File");
	p = pane_register(p, 0, &find_handle.c, "file write");
	if (!p)
		return Efail;
	attr_set_str(&p->attrs, "initial_path", path);
	call("attach-history", p, 0, NULL, "*File History*");
	return 1;
}

DEF_CMD(emacs_do_writefile)
{
	const char *path = ksuffix(ci, "emacs:write_file:");
	if (!ci->str || !path)
		return Efail;

	path = file_normalize(ci->focus, ci->str, path);
	if (!path)
		return Efail;
	if (call("doc:write-file", ci->focus, NO_NUMERIC, NULL, path) <= 0) {
		call("Message", ci->focus, 0, NULL,
		     strconcat(ci->focus, "Failed to write to ", path));
		return Efail;
	}
	return 1;
}

DEF_CMD(emacs_insertfile)
{
	/* Request to insert content of a file at point */
	struct pane *p;
	const char *path;
	char buf[PATH_MAX];
	char *e;
	int ret;

	ret = call("doc:insert-file", ci->focus, NO_NUMERIC);
	if (ret != Enoarg) {
		const char *doc;
		if (ret == Efail)
			/* Message already given */
			return ret;
		doc = pane_attr_get(ci->focus, "doc-name");
		if (doc)
			call("Message", ci->focus, 0, NULL,
			     strconcat(ci->focus, "Document ", doc,
				       " cannot receive insertions"));
		else
			call("Message", ci->focus, 0, NULL,
			     "This document cannot receive insertions");
		return Efail;
	}
	path = pane_attr_get(ci->focus, "dirname");
	if (path) {
		strcpy(buf, path);
		path = buf;
	}
	if (!path) {
		strcpy(buf, "/");
		path = buf;
	}
	e = buf + strlen(buf);
	if (e < buf + sizeof(buf)-1 && e > buf && e[-1] != '/') {
		*e++ = '/';
		*e++ = '\0';
	}

	p = call_ret(pane, "PopupTile", ci->focus, 0, NULL, "D2",
		     0, NULL, path);
	if (!p)
		return Efail;
	attr_set_str(&p->attrs, "prompt", "Insert File");
	attr_set_str(&p->attrs, "done-key",
		     strconcat(p, "emacs:insert_file:", path));
	call("doc:set-name", p, 0, NULL, "Insert File");
	p = pane_register(p, 0, &find_handle.c, "file");
	if (!p)
		return Efail;
	attr_set_str(&p->attrs, "initial_path", path);
	call("attach-history", p, 0, NULL, "*File History*");
	return 1;
}

DEF_CMD(emacs_do_insertfile)
{
	const char *path = ksuffix(ci, "emacs:insert_file:");
	int fd;

	if (!ci->str || !path)
		return Efail;

	path = file_normalize(ci->focus, ci->str, path);
	if (!path)
		return Efail;
	fd = open(path, O_RDONLY);
	if (fd >= 0) {
		if (call("doc:insert-file", ci->focus, fd) <= 0) {
			close(fd);
			call("Message", ci->focus, 0, NULL,
			     strconcat(ci->focus, "Failed to insert ", path));
			return Efail;
		}
		close(fd);
		return 1;
	} else {
		char *m = strconcat(ci->focus,
				    "Failed to inser file: ", path);
		call("Message", ci->focus, 0, NULL, m);
		return Efail;
	}
}

REDEF_CMD(emacs_file_complete)
{
	/* Extract a directory name and a basename from the document.
	 * Find a document for the directory and attach as a completing
	 * popup menu
	 */
	const char *str;
	const char *d, *b;
	int fd;
	struct pane *par, *pop, *docp, *p;
	struct call_return cr;
	char *type = ci->home->data;
	char *initial = attr_find(ci->home->attrs, "initial_path");
	int wholebuf = strcmp(type, "file") == 0;
	struct mark *st;

	if (!ci->mark)
		return Enoarg;

	st = mark_dup(ci->mark);
	call("doc:file", ci->focus, -1, st);

	str = call_ret(strsave, "doc:get-str", ci->focus, 0, st, NULL,
		       0, ci->mark);
	if (!str)
		return Einval;
	if (wholebuf) {
		d = str;
	} else {
		/* Need guess which part of the buf is the file name.
		 * This probably needs to be configurable, but lets
		 * decide the file name starts immediately after a
		 * space, or a '=' or ':' which is followed by a
		 * '/'.
		 */
		initial = "";
		d = str + strlen(str);
		while (d > str &&
		       !(d[-1] == ' ' ||
			 (strchr(":=", d[-1]) && d[0] == '/')))
			d -= 1;
	}
	d = file_normalize(ci->focus, d, initial);
	b = strrchr(d, '/');
	if (b) {
		b += 1;
		d = strnsave(ci->focus, d, b-d);
	} else {
		b = d;
		d = strsave(ci->focus, ".");
	}
	fd = open(d, O_DIRECTORY|O_RDONLY);
	if (fd < 0) {
		call("Message:modal", ci->focus, 0, NULL,
		     strconcat(ci->focus, "Cannot open directory \"", d, "\""));
		return Efail;
	}
	/* 32 means quiet */
	docp = call_ret(pane, "doc:open", ci->focus, fd, NULL, d, 32);
	close(fd);
	if (!docp)
		return Efail;
	pop = call_ret(pane, "PopupTile", ci->focus, 0, NULL, "DM1r");
	if (!pop)
		return Efail;
	par = home_call_ret(pane, docp, "doc:attach-view", pop,
			    0, NULL, "simple");
	if (!par)
		return Efail;

	attr_set_str(&par->attrs, "line-format", "%name%suffix");
	attr_set_str(&par->attrs, "heading", "");
	attr_set_str(&par->attrs, "done-key", "Replace");
	p = call_ret(pane, "attach-render-complete", par, 0, NULL, "format");
	if (!p)
		return Efail;
	cr = call_ret(all, "Complete:prefix", p, 1, NULL, b,
		      0, NULL, "format:plain");
	if (cr.s && cr.ret-1 > 1 && cr.s[strlen(cr.s)-1] == '/')
		/* Trailing slash on non-unique name must be removed */
		cr.s[strlen(cr.s)-1] = '\0';
	if (cr.s && (strlen(cr.s) <= strlen(b) && cr.ret-1 > 1)) {
		/* We need the dropdown - delete prefix and drop-down will
		 * insert result.
		 */
		struct mark *start;

		start = mark_dup(ci->mark);
		call("doc:char", ci->focus, -strlen(b), start);
		call("Replace", ci->focus, 1, start);
		mark_free(start);

		return 1;
	}
	if (cr.s) {
		/* Replace 'b' with the result. */
		struct mark *start;

		start = mark_dup(ci->mark);
		call("doc:char", ci->focus, -strlen(b), start);
		call("Replace", ci->focus, 1, start, cr.s);
		mark_free(start);
	} else {
		call("Message:modal", ci->focus, 0, NULL,
		     strconcat(ci->focus, "No completion found for \"", b, "\"",
			       " in \"", d, "\""));
	}
	/* Now need to close the popup */
	pane_close(pop);
	return 1;
}

DEF_CMD(emacs_finddoc)
{
	struct pane *p, *par;

	if (ksuffix(ci, "Doc Found:")[0] == 0) {
		struct pane *dflt;
		char *defname = NULL;

		dflt = call_ret(pane, "docs:choose", ci->focus);
		if (dflt)
			defname = pane_attr_get(dflt, "doc-name");

		p = call_ret(pane, "PopupTile", ci->focus, 0, NULL, "D2",
			     0, NULL, "");
		if (!p)
			return Efail;

		if (defname)
			attr_set_str(&p->attrs, "default", defname);
		if (ksuffix(ci, "K:CX44")[0]) {
			attr_set_str(&p->attrs, "prompt",
				     "Find Document Popup");
			attr_set_str(&p->attrs, "done-key",
				     "Doc Found:Popup");
		} else if (ksuffix(ci, "K:CX4")[0]) {
			attr_set_str(&p->attrs, "prompt",
				     "Find Document Other Window");
			attr_set_str(&p->attrs, "done-key",
				     "Doc Found:Other Window");
		} else {
			attr_set_str(&p->attrs, "prompt", "Find Document");
			attr_set_str(&p->attrs, "done-key",
				     "Doc Found:This Window");
		}
		call("doc:set-name", p, 0, NULL, "Find Document", -1);

		pane_register(p, 0, &find_handle.c, "doc");
		return 1;
	}

	if (!ci->str)
		/* Aborted */
		return Efail;

	p = call_ret(pane, "docs:byname", ci->focus, 0, NULL, ci->str);
	if (!p)
		return Efail;

	if (strcmp(ci->key, "Doc Found:Other Window") == 0) {
		struct pane *this = call_ret(pane, "ThisPane", ci->focus);
		par = home_call_ret(pane, ci->focus, "DocPane", p);
		if (par && par != this) {
			pane_take_focus(par);
			return 1;
		}
		par = call_ret(pane, "OtherPane", ci->focus);
	} else if (strcmp(ci->key, "Doc Found:Popup") == 0) {
		par = call_ret(pane, "PopupTile", ci->focus, 0, NULL, "MD3tsa");
	} else
		par = call_ret(pane, "ThisPane", ci->focus);
	if (!p || !par)
		return Efail;

	p = home_call_ret(pane, p, "doc:attach-view", par, 1);
	if (p)
		pane_take_focus(p);
	return p ? 1 : Efail;
}

REDEF_CMD(emacs_doc_complete)
{
	/* Extract a document name from the document.
	 * Attach the 'docs' document as a completing popup menu
	 */
	char *str;
	struct pane *pop, *p = NULL;
	struct call_return cr;

	if (!ci->mark)
		return Enoarg;

	str = call_ret(strsave, "doc:get-str", ci->focus);
	if (!str)
		return Einval;
	pop = call_ret(pane, "PopupTile", ci->focus, 0, NULL, "DM1r");
	if (!pop)
		return Efail;
	p = call_ret(pane, "docs:complete", pop);
	if (!p)
		return Efail;
	cr = call_ret(all, "Complete:prefix", p, 1, NULL, str);
	if (cr.s && (strlen(cr.s) <= strlen(str) && cr.ret - 1 > 1)) {
		/* We need the dropdown */
		struct mark *start;

		start = mark_dup(ci->mark);
		call("doc:set-ref", ci->focus, 1, start);

		call("Replace", ci->focus, 1, start);
		mark_free(start);
		return 1;
	}
	if (cr.s) {
		/* Replace the prefix with the new value */
		struct mark *start;

		start = mark_dup(ci->mark);
		call("doc:set-ref", ci->focus, 1, start);

		call("Replace", ci->focus, 1, start, cr.s);
		mark_free(start);
	}
	/* Now need to close the popup */
	pane_close(pop);
	return 1;
}

DEF_CMD(emacs_viewdocs)
{
	struct pane *p, *par;
	struct pane *docs;

	docs = call_ret(pane, "docs:byname", ci->focus, 0, NULL, "*Documents*");
	if (!docs)
		return Efail;

	if (ksuffix(ci, "K:CX44")[0]) {
		par = call_ret(pane, "PopupTile", ci->focus, 0, NULL, "MD3tsa");
	} else if (ksuffix(ci, "K:CX4")[0]) {
		struct pane *this = call_ret(pane, "ThisPane", ci->focus);
		par = home_call_ret(pane, ci->focus, "DocPane", docs);
		if (par && par != this) {
			pane_take_focus(par);
			return 1;
		}
		par = call_ret(pane, "OtherPane", ci->focus);
	} else {
		par = call_ret(pane, "ThisPane", ci->focus);
	}

	if (!par)
		return Efail;

	p = home_call_ret(pane, docs, "doc:attach-view", par, 1);
	return !!p;
}

struct pcb {
	struct command c;
	struct pane *doc safe;
	struct pane *p;
};

DEF_CMD(choose_pane)
{
	struct pcb *cb = container_of(ci->comm, struct pcb, c);
	struct pane *d;

	if (cb->p || !ci->str)
		return 1;
	d = call_ret(pane, "docs:byname", ci->focus, 0, NULL, ci->str);
	if (d)
		cb->p = home_call_ret(pane, ci->focus, "DocPane", d);
	if (cb->p)
		home_call(cb->doc, "doc:attach-view", cb->p, 1);
	return 1;
}

DEF_CB(shellcb)
{
	char *str;

	if (strcmp(ci->key, "cb:timer") == 0) {
		/* If focus has moved, don't show shell window,
		 * probably a popup was requested by command.
		 */
		if (!pane_has_focus(ci->home))
			/* call back when lines or eof */
			return 2;
	}
	if (strcmp(ci->key, "cb:eof") != 0) {
		struct pane *par;
		if (ci->str && strchr(ci->str, 'P'))
			par = call_ret(pane, "PopupTile", ci->home, 0, NULL, "MD3tsa");
		else
			par = call_ret(pane, "OtherPane", ci->home);
		if (par)
			home_call(ci->focus, "doc:attach-view", par, 1);
		return 1;
	}
	str = call_ret(str, "doc:get-str", ci->focus);
	if (!str || !*str) {
		free(str);
		if (ci->num == 0)
			asprintf(&str, "(shell command completed with no output)");
		else if (ci->num > 0)
			asprintf(&str, "(shell command completed with no output (exit code %d))", ci->num);
		else
			asprintf(&str, "(shell command completed with no output (signalled %d))", -ci->num);
	}
	call("Message", ci->home, 0, NULL, str);
	free(str);
	return 1;
}

DEF_CB(shell_insert_cb)
{
	char *str = call_ret(str, "doc:get-str", ci->focus);
	struct mark *mk = call_ret(mark2, "doc:point", ci->home);

	if (call("selection:clear", ci->home, 3, mk) >= 1)
		call("Replace", ci->home, 1, mk);
	call("Replace", ci->home, 0, NULL, str);
	free(str);
	return 1;
}

DEF_CMD(emacs_shell)
{
	char *name = "*Shell Command Output*";
	struct pane *p, *doc, *par, *sc;
	char *path, *input = NULL;
	struct pcb cb;
	bool interpolate, pipe, popup;

	if (strcmp(ci->key, "Shell Command") != 0) {
		char *dirname;
		char aux[4] = "";

		p = call_ret(pane, "PopupTile", ci->focus, 0, NULL, "D2", 0, NULL,
			     ci->str ?: "");
		if (!p)
			return Efail;
		dirname = call_ret(strsave, "get-attr", ci->focus, 0, NULL, "dirname");
		attr_set_str(&p->attrs, "dirname", dirname ?: ".");
		attr_set_str(&p->attrs, "prompt", "Shell command");
		if (ci->num == 0 || ci->num == 1)
			strcat(aux, "i"); // interpolate
		if (ci->num == 4)
			strcat(aux, "P"); // popup
		if (strcmp(ci->key, "K:A-|") == 0)
			strcat(aux, "p"); // pipe from selection
		attr_set_str(&p->attrs, "popup-aux", aux);
		attr_set_str(&p->attrs, "done-key", "Shell Command");
		call("doc:set-name", p, 0, NULL, "Shell Command", -1);
		p = call_ret(pane, "attach-history", p, 0, NULL, "*Shell History*");
		if (p)
			p = pane_register(p, 0, &find_handle.c, "shellcmd");
		if (!p)
			return Efail;
		if (ci->comm2)
			comm_call(ci->comm2, "cb", p);
		return 1;
	}
	if (!ci->str) {
		call("Message", ci->focus, 0, NULL, "Shell command aborted");
		return 1;
	}

	interpolate = ci->str2 && strchr(ci->str2, 'i');
	pipe = ci->str2 && strchr(ci->str2, 'p');
	popup = ci->str2 && strchr(ci->str2, 'P');

	if (pipe) {
		struct mark *mk = call_ret(mark2, "doc:point", ci->focus);
		if (mk) {
			input = call_ret(str, "doc:get-str", ci->focus,
					 0, NULL, NULL, 0, mk);
			/* make the selection replacable */
			attr_set_int(&mk->attrs, "selection:active", 3);
		}
	}
	doc = call_ret(pane, "doc:from-text", ci->focus, 0, NULL, name, 0, NULL,
		       input ?: "");
	free(input);
	if (!doc)
		return Efail;

	path = pane_attr_get(ci->focus, "dirname");
	attr_set_str(&doc->attrs, "dirname", path);

	/* shellcmd is attached directly to the document, not in the view
	 * stack.  It is go-between for document and external command.
	 * We don't need a doc attachment as no point is needed - we
	 * always insert at the end.
	 */
	sc = call_ret(pane, "attach-shellcmd", doc, pipe ? 22 : 4,
		      NULL, ci->str, 0, NULL, path);
	if (!sc)
		call("doc:replace", doc, 0, NULL,
		     "Failed to run command - sorry\n");
	if (call("text-search", doc, 0, NULL, "diff|(stg|git).*show",
		 0, NULL, ci->str) > 0)
		attr_set_str(&doc->attrs, "view-default", "diff");

	/* Close old shell docs, but if one is visible in current frame, replace
	 * it with doc
	 */
	cb.c = choose_pane;
	cb.doc = doc;
	cb.p = NULL;
	call_comm("editor:notify:shell-reuse", ci->focus, &cb.c);
	if (!cb.p) {
		/* choose_pane didn't attach, so set a callback to do
		 * it when there is enough content.
		 */
		if (sc) {
			/* If it take more than 500msec, or includes 2
			 * or more lines, we'll show in a pane, else
			 * just show as a message.
			 */
			if (!interpolate)
				home_call_comm(sc, "shellcmd:set-callback",
					       ci->focus, &shellcb,
					       500, NULL, popup ? "P": "",
					       2);
		} else {
			if (popup)
				par = call_ret(pane, "PopupTile", ci->focus,
					       0, NULL, "MD3tsa");
			else
				par = call_ret(pane, "OtherPane", ci->focus);
			if (!par)
				return Efail;
			home_call(doc, "doc:attach-view", par, 1);
		}
	}
	if (sc && interpolate)
		/* Need a callback when the pipe command finished */
		home_call_comm(sc, "shellcmd:set-callback", ci->focus,
			       &shell_insert_cb);

	return 1;
}

DEF_CMD(emacs_num)
{
	int rpt = ci->num;
	const char *last = ksuffix(ci, "K:A-");
	int neg = 0;

	if (rpt < 0) {
		neg = 1;
		rpt = -rpt;
	}
	if (rpt == NO_NUMERIC)
		rpt = 0;

	rpt = rpt * 10 + *last - '0';

	call("Mode:set-num", ci->focus, neg ? -rpt : rpt);
	call("Mode:set-num2", ci->focus, ci->num2);
	return 1;
}

DEF_CMD(emacs_neg)
{
	call("Mode:set-num", ci->focus, - ci->num);
	call("Mode:set-num2", ci->focus, ci->num2);
	return 1;
}

DEF_CMD(emacs_prefix)
{
	/* as a generic arg (ctrl-U) which is positive and
	 * as as a repeat-count of 4, but is different to 4.
	 * I should probably allow digits to alter the number.
	 */
	call("Mode:set-num", ci->focus, 4);
	return 1;
}

DEF_CMD(emacs_kill_doc)
{
	if (ci->num <= 0 || ci->num == NO_NUMERIC) {
		/* Check if modified. */
		char *m = pane_attr_get(ci->focus, "doc-modified");
		char *f = NULL;
		if (m && strcmp(m, "yes") == 0)
			f = pane_attr_get(ci->focus, "filename");
		if (f) {
			call("Message:modal", ci->focus, 0, NULL,
			     "Document is modified - please save or give prefix arg");
			return 1;
		}
	}
	return call("doc:destroy", ci->focus);
}

DEF_CMD(emacs_revisit)
{
	return call("doc:load-file", ci->focus, 2, NULL, NULL, -1);
}

DEF_CMD(emacs_save_all)
{
	int ret = call("docs:save-all", ci->focus, 0, NULL, NULL, 1);

	if (ret == 1) {
		call("Message", ci->focus, 0, NULL, "No files need to be saved.");
		return 1;
	}
	if (ci->num == NO_NUMERIC) {
		struct pane *p = call_ret(pane, "PopupTile", ci->focus, 0, NULL, "DM");
		if (p)
			return call("docs:show-modified", p);
	}
	return call("docs:save-all", ci->focus);
}

DEF_CMD(emacs_reposition)
{
	struct mark *m;
	int repoint = attr_find_int(ci->focus->attrs, "emacs-repoint");

	if (repoint != -1) {
		/* Move point to end of display, if that is in
		 * the right direction.  That will mean it has moved
		 * off the display.
		 */
		m = mark_at_point(ci->focus, NULL, MARK_UNGROUPED);
		if (m) {
			struct mark *m2 = mark_dup(m);
			call("Move-CursorXY", ci->focus, 0, m, NULL,
			     0, NULL, NULL,
			     INT_MAX, repoint < 0 ? ci->focus->h-1 : 0);
			if (repoint < 0)
				/* can only move point backwards */
				if (m->seq < m2->seq)
					call("Move-to", ci->focus, 0, m);
			if (repoint > 0)
				/* can only move point forwards */
				if (m->seq > m2->seq)
					call("Move-to", ci->focus, 0, m);
			mark_free(m);
			mark_free(m2);
		}
		attr_set_str(&ci->focus->attrs, "emacs-repoint", NULL);
	}
	return Efallthrough;
}

DEF_CMD(emacs_start_search)
{
	struct pane *p = NULL, *hp;
	int mode = 0;

	hp = call_ret(pane, "attach-emacs-search-highlight", ci->focus);

	if (hp)
		p = call_ret(pane, "PopupTile", hp, 0, NULL, "TR2",
			     0, NULL, "");

	if (!p)
		return Efail;
	home_call(hp, "highlight:set-popup", p);

	attr_set_str(&p->attrs, "prompt", "Search");
	attr_set_str(&p->attrs, "done-key", "Search String");

	hp = call_ret(pane, "attach-history", p, 0, NULL, "*Search History*");
	if (hp)
		p = hp;

	call("doc:set-name", p, 0, NULL, "Search", -1);
	if (strcmp(ci->key, "K:C-R") == 0)
		mode |= 1;
	if (strcmp(ci->key, "K:A-%") == 0)
		mode |= 2;
	call_ret(pane, "attach-emacs-search", p, mode);

	return 1;
}

DEF_CMD(emacs_command)
{
	struct pane *p;

	p = call_ret(pane, "PopupTile", ci->focus, 0, NULL, "D2", 0, NULL, "");
	if (!p)
		return Efail;
	attr_set_str(&p->attrs, "prompt", "Cmd");
	attr_set_str(&p->attrs, "done-key", "emacs:command");
	call("doc:set-name", p, 0, NULL, "K:Ax command", -1);
	p = call_ret(pane, "attach-history", p, 0, NULL, "*Command History*");
	if (p)
		pane_register(p, 0, &find_handle.c, "cmd");
	return 1;
}

DEF_CMD(emacs_do_command)
{
	char *cmd = NULL;
	int ret;

	if (!ci->str)
		/* Aborted */
		return Efail;
	asprintf(&cmd, "interactive-cmd-%s", ci->str);
	if (!cmd)
		return Efail;
	ret = call(cmd, ci->focus, 0, ci->mark);
	free(cmd); cmd = NULL;
	if (ret == 0) {
		asprintf(&cmd, "Command %s not found", ci->str);
		call("Message", ci->focus, 0, NULL, cmd);
	} else if (ret < 0) {
		asprintf(&cmd, "Command %s Failed", ci->str);
		call("Message", ci->focus, 0, NULL, cmd);
	}
	free(cmd);
	return 1;
}

DEF_CB(take_cmd)
{
	struct call_return *cr = container_of(ci->comm, struct call_return, c);
	const char *cmd;

	if (!ci->str)
		return Enoarg;
	cmd = ci->str + 16;
	if (cr->p) {
		struct mark *m = vmark_new(cr->p, MARK_UNGROUPED, NULL);
		call("doc:list-add", cr->p, 0, m);
		call("doc:set-attr", cr->p, 0, m, "cmd", 0, NULL, cmd);
	}
	return 1;
}

REDEF_CMD(emacs_cmd_complete)
{
	char *s;
	struct pane *doc = NULL, *pop = NULL, *p;
	struct call_return cr;

	if (!ci->mark)
		return Enoarg;
	s = call_ret(strsave, "doc:get-str", ci->focus);
	if (!s)
		s = "";
	doc = call_ret(pane, "attach-doc-list", ci->focus);
	if (!doc)
		return Efail;
	call("doc:set-name", doc, 0, NULL, "*Command List*");
	call("doc:set:autoclose", doc, 1);
	attr_set_str(&doc->attrs, "render-simple", "format");
	attr_set_str(&doc->attrs, "heading", "");
	attr_set_str(&doc->attrs, "line-format", "%cmd");
	cr.c = take_cmd;
	cr.p = doc;
	call_comm("keymap:list", ci->focus, &cr.c,
		  0, NULL, "interactive-cmd-");
	call("doc:list-sort", doc, 0, NULL, "cmd");
	pop = call_ret(pane, "PopupTile", ci->focus, 0, NULL, "DM1r");
	if (!pop)
		goto fail;
	p = home_call_ret(pane, cr.p, "doc:attach-view", pop, -1, NULL, "simple");
	if (!p)
		goto fail_pop;
	attr_set_str(&p->attrs, "done-key", "Replace");
	p = call_ret(pane, "attach-render-complete", p);
	if (!p)
		goto fail_pop;
	cr = call_ret(all, "Complete:prefix", p, 1, NULL, s);
	if (cr.s && strlen(cr.s) <= strlen(s) && cr.ret-1 > 1) {
		/* We need the dropdown - delete prefix and drop-down will
		 * insert result.
		 */
		struct mark *start = mark_dup(ci->mark);
		call("Move-Char", ci->focus, -strlen(s), start);
		call("Replace", ci->focus, 1, start);
		mark_free(start);
		return 1;
	}
	if (cr.s) {
		/* Replace 's' with the result */
		struct mark *start = mark_dup(ci->mark);
		call("Move-Char", ci->focus, -strlen(s), start);
		call("Replace", ci->focus, 1, start, cr.s);
		mark_free(start);
	}
	/* Now need to close the popup */
	pane_close(pop);
	pane_close(doc);
	return 1;

fail_pop:
	pane_close(pop);
fail:
	pane_close(doc);
	return Efail;
}

DEF_CMD(emacs_version)
{
	char *v = NULL;

	asprintf(&v, "%s ; emacs-mode - v" VERSION " - " VERS_DATE, edlib_version);
	if (v)
		call("Message", ci->focus, 0, NULL, v);
	free(v);
	return 1;
}

DEF_CMD(emacs_log)
{
	/* View the debug log */
	struct pane *p, *doc;

	doc = call_ret(pane, "docs:byname", ci->focus, 0, NULL, "*Debug Log*");
	if (!doc) {
		call("interactive-cmd-view-log", ci->focus);
		doc = call_ret(pane, "docs:byname", ci->focus, 0, NULL,
			     "*Debug Log*");
	}
	if (!doc)
		return Efail;
	p = call_ret(pane, "ThisPane", ci->focus);
	if (p)
		home_call(doc, "doc:attach-view", p, 1);
	return 1;
}

DEF_CMD(emacs_mark)
{
	struct mark *m = call_ret(mark2, "doc:point", ci->focus);

	call("selection:clear", ci->focus, 0, m);

	call("Move-to", ci->focus, 1);
	m = call_ret(mark2, "doc:point", ci->focus);
	if (m)
		/* ci->num == 1 means replacable */
		call("selection:set", ci->focus,
		     ci->num == 1 ? 3 : 1, m);
	return 1;
}

DEF_CMD(emacs_swap_mark)
{
	struct mark *mk = call_ret(mark2, "doc:point", ci->focus);
	struct mark *m;

	if (!mk)
		return 1;
	m = mark_dup(mk);
	call("Move-to", ci->focus, 1); /* Move mark to point */
	call("Move-to", ci->focus, 0, m); /* Move point to old mark */
	call("selection:set", ci->focus,
	     ci->num == 1 ? 3 : 1, mk);
	mark_free(m);
	return 1;
}

DEF_CMD(emacs_wipe)
{
	/* Delete text from point to mark */
	struct mark *mk = call_ret(mark2, "doc:point", ci->focus);
	char *str;
	int ret;

	if (!mk)
		return 1;
	/* Remove any selection so it cannot be claimed and so replace this copy */
	call("selection:claim", ci->focus);
	call("selection:discard", ci->focus);

	str = call_ret(strsave, "doc:get-str", ci->focus, 0, NULL, NULL, 0, mk);
	if (str && *str)
		call("copy:save", ci->focus, 0, NULL, str);
	ret = call("Replace", ci->focus, 1, mk);
	/* Clear mark */
	call("selection:clear", ci->focus, 0, mk);

	return ret;
}

DEF_CMD(emacs_copy)
{
	/* copy text from point to mark */
	struct mark *mk = call_ret(mark2, "doc:point", ci->focus);
	char *str;

	if (!mk)
		return 1;
	/* Remove any selection so it cannot be claimed and so replace this copy */
	call("selection:claim", ci->focus);
	call("selection:discard", ci->focus);

	str = call_ret(strsave, "doc:get-str", ci->focus, 0, NULL, NULL, 0, mk);
	if (str && *str)
		call("copy:save", ci->focus, 0, NULL, str);
	/* Clear current highlight */
	call("selection:clear", ci->focus, 0, mk);
	return 1;
}

DEF_CMD(emacs_yank)
{
	int n = RPT_NUM(ci);
	char *str;
	struct mark *mk;
	struct mark *m = NULL;

	/* If there is a selection elsewhere, we want to commit it */
	call("selection:discard", ci->focus);
	call("selection:commit", ci->focus);

	str = call_ret(strsave, "copy:get", ci->focus, n - 1);
	if (!str || !*str)
		return 1;
	/* If mark exists and is active, replace marked regions */
	mk = call_ret(mark2, "doc:point", ci->focus);
	if (mk && call("selection:clear", ci->focus, 0, mk) >= 1) {
		char *str2 = call_ret(strsave, "doc:get-str", ci->focus,
				      0, NULL, NULL, 0, mk);
		if (str2 && *str2)
			call("copy:save", ci->focus, 0, NULL, str2);
		m = mark_dup(mk);
	}

	call("Move-to", ci->focus, 1);
	call("Replace", ci->focus, 1, m, str);
	mark_free(m);
	mk = call_ret(mark2, "doc:point", ci->focus);
	call("Mode:set-num2", ci->focus, N2_yank);
	return 1;
}

DEF_CMD(emacs_yank_pop)
{
	struct mark *mk, *m;
	char *str;
	int num = N2a(ci);

	if (N2(ci) != N2_yank)
		return Einval;
	mk = call_ret(mark2, "doc:point", ci->focus);
	if (!mk)
		return 1;
	num += 1;
	str = call_ret(strsave, "copy:get", ci->focus, num);
	if (!str) {
		num = 0;
		str = call_ret(strsave, "copy:get", ci->focus, num);
	}
	if (!str)
		return Efail;
	m = mark_dup(mk);
	mark_step(m, 0);
	call("Replace", ci->focus, 1, mk, str);
	call("Move-to", ci->focus, 1, m);
	mark_free(m);
	call("Mode:set-num2", ci->focus, (num << 16) | N2_yank);
	return 1;
}

DEF_CMD(emacs_selection_menu_add)
{
	struct pane *p = ci->focus;
	call("menu-add", p, 0, NULL, "Cut", 0, NULL, ":C-W");
	call("menu-add", p, 0, NULL, "Copy", 0, NULL, ":A-w");
	call("menu-add", p, 0, NULL, "Paste-in", 0, NULL, ":C-Y");
	return Efallthrough;
}

DEF_CMD(emacs_goto_line)
{
	if (ci->num == NO_NUMERIC)
		return 1;
	call("CountLines", ci->focus, ci->num, ci->mark, "goto:line");
	call("Move-View-Pos", ci->focus, 0, ci->mark);
	return 1;
}

DEF_CMD(emacs_next_match)
{
	call("Mode:set-num2", ci->focus, N2_match);
	call("Message:modal", ci->focus, 0, NULL, "Type ` to search again");
	return call("interactive-cmd-next-match", ci->focus, ci->num, NULL, NULL,
		    strcmp(ci->key, "K-`") == 0);
}

DEF_CMD(emacs_match_again)
{
	if (N2(ci) != N2_match)
		return Efallthrough;
	else
		return emacs_next_match_func(ci);
}

DEF_CMD(emacs_make)
{
	call("interactive-cmd-make", ci->focus,
	     ci->num, ci->mark, NULL,
	     strcmp(ci->key, "K:CC:C-M") == 0);
	return 1;
}

DEF_CMD(emacs_paste)
{
	char *str;

	/* First commit the selection, then collect it */
	call("selection:commit", ci->focus);
	str = call_ret(strsave, "copy:get", ci->focus);

	call("Move-CursorXY", ci->focus,
	     0, NULL, NULL, 0, NULL, NULL, ci->x, ci->y);

	if (!str || !*str)
		return 1;

	call("Replace", ci->focus, 0, NULL, str);

	pane_take_focus(ci->focus);

	return 1;
}

DEF_CMD(emacs_readonly)
{
	char *ro;

	ro = pane_attr_get(ci->focus, "doc-readonly");

	if (ro && strcmp(ro,"yes") == 0)
		call("doc:set:readonly", ci->focus, 0);
	else
		call("doc:set:readonly", ci->focus, 1);
	return 1;
}

DEF_CMD(emacs_shift)
{
	int rpt = ci->num;
	int shift;

	shift = pane_attr_get_int(ci->focus, "render-wrap", -1);
	if (strcmp(ci->key, "K:CX->") == 0 || strcmp(ci->key, "K->") == 0)
		rpt = -rpt;
	if (rpt == NO_NUMERIC) {
		if (shift < 0)
			shift = 0;
		else
			shift += 8;
	} else if (rpt == -NO_NUMERIC) {
		if (shift >= 8)
			shift -= 8;
		else if (shift > 0)
			shift = 0;
		else
			shift = -1;
	} else if (rpt >= 0) {
		if (shift < 0)
			shift = 0;
		shift += rpt;
	} else {
		if (shift > 0 && shift + rpt < 0)
			shift = 0;
		else
			shift += rpt;
	}
	if (shift < 0)
		attr_set_str(&ci->focus->attrs, "render-wrap", "yes");
	else if (shift > 0)
		attr_set_int(&ci->focus->attrs, "render-wrap", shift);
	else if (rpt > 0)
		attr_set_str(&ci->focus->attrs, "render-wrap", "0 auto");
	else
		/* When reducing shift to zero, don't enable auto */
		attr_set_int(&ci->focus->attrs, "render-wrap", 0);
	call("view:changed", ci->focus);
	call("Mode:set-num2", ci->focus, N2_shift);
	call("Message:modal", ci->focus, 0, NULL, "Type < or > to shift again");
	return 1;
}

DEF_CMD(emacs_shift_again)
{
	if (N2(ci) != N2_shift)
		return Efallthrough;
	else
		return emacs_shift_func(ci);
}

DEF_CMD(emacs_growx)
{
	if (ci->key[strlen(ci->key)-1] == '}')
		call("Tile:x+", ci->focus, ci->num);
	else
		call("Tile:x-", ci->focus, ci->num);
	call("Mode:set-num2", ci->focus, N2_growx);
	call("Message:modal", ci->focus, 0, NULL, "Type { or } to grow again");
	return 1;
}

DEF_CMD(emacs_growx_again)
{
	if (N2(ci) != N2_growx)
		return Efallthrough;
	else
		return emacs_growx_func(ci);
}

DEF_CMD(emacs_scale_relative)
{
	struct pane *p = ci->focus;
	char *sc = pane_attr_get(p, "scale:M");
	int scale = 0;
	char num[20];

	if (!sc) {
		call("Message:modal", p, 0, NULL,
		     "Cannot zoom display with fixed-sized font");
		return 1;
	}
	sc = pane_attr_get(p, "scale");
	if (sc && strchr(sc, 'x')) {
		call("Message:modal", p, 0, NULL,
		     "Cannot zoom display with fixed layout");
		return 1;
	}

	if (sc)
		scale = atoi(sc);
	if (scale <= 0)
		scale = 1000;
	if (ci->key[strlen(ci->key)-1] == '-')
		scale = 10 * scale / 12;
	else
		scale = 12 * scale / 10;
	snprintf(num, sizeof(num)-1, "%d", scale);
	call("Window:set:scale", p, 0, NULL, num);
	return 1;
}

DEF_CMD(emacs_curs_pos)
{
	struct mark *c;
	int col = 0;
	int chars = 0;
	wint_t ch, nxt;
	char *msg = NULL;

	if (!ci->mark)
		return Enoarg;
	c = mark_dup(ci->mark);
	nxt = doc_following(ci->focus, c);

	while ((ch = doc_prev(ci->focus, c)) != WEOF && !is_eol(ch))
		;
	while (mark_ordered_not_same(c, ci->mark)) {
		ch = doc_next(ci->focus, c);
		if (is_eol(ch)) {
			col = 0;
			chars = 0;
		} else if (ch == '\t') {
			col = (col|7)+1;
			chars += 1;
		} else {
			col += 1;
			chars += 1;
		}
	}
	mark_free(c);
	asprintf(&msg, "Cursor at column %d (%d chars), char is %d (0x%x)",
		 col, chars, nxt, nxt);
	call("Message", ci->focus, 0, NULL, msg);
	free(msg);
	return 1;
}

DEF_CMD(emacs_word_count)
{
	struct mark *mk;
	struct pane *p = ci->focus;
	char *msg = NULL;
	int wp;

	if (!ci->mark)
		return Enoarg;

	mk = call_ret(mark2, "doc:point", p);
	if (mk && attr_find_int(mk->attrs, "selection:active") <= 0)
		mk = NULL;
	call("CountLines", p, 0, ci->mark);
	wp = attr_find_int(ci->mark->attrs, "word");
	if (mk) {
		int wm;
		call("CountLines", p, 0, mk);
		wm = attr_find_int(mk->attrs, "word");
		asprintf(&msg, "%d words in region", abs(wp-wm));
	} else {
		int wd = pane_attr_get_int(p, "words", 0);
		asprintf(&msg, "After word%s %d of %d", wp==2?"":"s", wp-1, wd);
	}
	call("Message", p, 0, NULL, msg);
	free(msg);
	return 1;
}

DEF_CMD(emacs_fill)
{
	struct mark *mk = call_ret(mark2, "doc:point", ci->focus);
	struct mark *p = call_ret(mark, "doc:point", ci->focus);
	struct pane *p2;

	if (call("selection:clear", ci->focus, 0, mk, NULL, 0, p) == Efalse)
		mk = NULL;

	if (strcmp(ci->key, "K:A-q") == 0) {
		if (call("fill-paragraph", ci->focus, ci->num, p, NULL,
			 0, mk) == Efallthrough) {
			p2 = call_ret(pane, "attach-textfill", ci->focus);
			if (p2)
				call("fill-paragraph", p2, ci->num, p, NULL,
				     0, mk);
		}
	} else {
		/* Don't try to load anything, the file-type should
		 * have loaded something if relevant
		 */
		if (call("reindent-paragraph", ci->focus, ci->num, p, NULL,
			 0, mk) == 0)
			call("Message", ci->focus, 0, NULL,
			     "Reindent not supported on the document.");
	}
	return 1;
}

DEF_CMD(emacs_abbrev)
{
	call("attach-abbrev", ci->focus);
	return 1;
}

DEF_CMD(emacs_showinput)
{
	struct pane *p, *doc;

	if (call("input:log", ci->focus) <= 0) {
		call("Message", ci->focus, 0, NULL,
		     "Cannot get log of recent input.");
		return Efail;
	}

	doc = call_ret(pane, "docs:byname", ci->focus, 0, NULL, "*Debug Log*");
	if (!doc) {
		call("interactive-cmd-view-log", ci->focus);
		doc = call_ret(pane, "docs:byname", ci->focus, 0, NULL,
			     "*Debug Log*");
	}
	if (!doc)
		return Efail;
	p = call_ret(pane, "PopupTile", ci->focus, 0, NULL, "DM");
	if (p) {
		p = home_call_ret(pane, doc, "doc:attach-view", p, 1);
		if (p)
			call("doc:file", p, 1);
	}
	return 1;
}

DEF_CMD(emacs_macro_start)
{
	int ret;

	ret = call("macro:capture", ci->focus);
	if (ret == Efalse)
		call("Message", ci->focus, 0, NULL,
		     "Macro capture already happening");
	else if (ret <= 0)
		call("Message", ci->focus, 0, NULL,
		     "Macro facility not available");
	return ret;
}

DEF_CMD(emacs_macro_stop)
{
	int ret;

	ret = call("macro:finished", ci->focus, 2);
	if (ret > 0)
		call("Message", ci->focus, 0, NULL,
		     "Macro successfully created.");
	else if (ret == Efalse)
		call("Message", ci->focus, 0, NULL,
		     "No macro being created.");
	else
		call("Message", ci->focus, 0, NULL,
		     "Failure creating macro.");
	return ret;
}

DEF_CMD(emacs_macro_run)
{
	int cnt = RPT_NUM(ci);

	if (strcmp(ci->key, "K-e") == 0 && N2(ci) != N2_runmacro)
		return Efallthrough;

	if (cnt < 1)
		cnt = 1;
	while (cnt >= 1 &&
	       call("macro:replay", ci->focus, 1) > 0)
		cnt -= 1;

	call("Mode:set-num2", ci->focus, N2_runmacro);
	call("Message:modal", ci->focus, 0, NULL, "Type 'e' to repeat macro");
	return cnt < 1 ? 1 : Efail;
}

struct bb {
	struct buf b;
	struct command c;
	bool first;
	int count;
};
static const char spell_choices[] =
	"0123456789-=;,/ABCDEFGHIJKLMNOPQRSTUVWXYZ";
DEF_CB(get_suggestion)
{
	struct bb *b = container_of(ci->comm, struct bb, c);

	if (!ci->str)
		return Enoarg;

	if (b->first)
		buf_concat(&b->b, " - (a)ccept, (i)nsert -     ");
	else
		buf_concat(&b->b, ", ");
	b->first = False;
	if (b->count < (int)sizeof(spell_choices)-1) {
		buf_append(&b->b, '(');
		buf_append(&b->b, spell_choices[b->count]);
		buf_append(&b->b, ')');
	}
	b->count += 1;
	buf_concat(&b->b, ci->str);
	return 1;
}

DEF_CMD(emacs_spell)
{
	struct mark *st;
	char *word;
	int ret;
	int rpt = RPT_NUM(ci);

	if (!ci->mark)
		return Enoarg;

	/* We always find a word that is partly *after* the given
	 * make, but we want to find the word before point, so step
	 * back.
	 */
	doc_prev(ci->focus, ci->mark);
again:
	if (ci->num != NO_NUMERIC)
		/* As a repeat-count was given, only look at intersting words */
		call("Spell:NextWord", ci->focus, 0, ci->mark);
	st = mark_dup(ci->mark);
	word = call_ret(str, "Spell:ThisWord", ci->focus, 0, ci->mark, NULL, 0, st);
	if (!word || !*word) {
		/* No word found */
		call("Message", ci->focus, 0, NULL,
		     "Spell check reached end-of-file");
		call("Spell:Save", ci->focus);
		free(word);
		mark_free(st);
		return 1;
	}
	ret = call("Spell:Check", ci->focus, 0, NULL, word);
	if (ret > 0) {
		rpt -= 1;
		mark_free(st);
		if (rpt)
			goto again;
		call("Message", ci->focus, 0, NULL,
		     strconcat(ci->focus, "\"", word,
			       "\" is a correct spelling."));
	} else if (ret == Efalse) {
		struct bb b;
		buf_init(&b.b);
		buf_concat(&b.b, "\"");
		buf_concat(&b.b, word);
		buf_concat(&b.b, "\" is NOT correct");
		b.count = 0;
		b.first = True;
		b.c = get_suggestion;
		call_comm("Spell:Suggest", ci->focus, &b.c,
			  0, NULL, word);
		if (b.first) {
			buf_concat(&b.b, " ... no suggestions");
		} else {
			attr_set_str(&ci->focus->attrs, "spell:last-error",
				     word);
			attr_set_str(&ci->focus->attrs, "spell:suggestions",
				     buf_final(&b.b));
			attr_set_int(&ci->focus->attrs, "spell:offset", 0);
		}
		call("Mode:set-all", ci->focus, rpt-1, NULL, ":Spell");
		call("Message:modal", ci->focus, 0, NULL, buf_final(&b.b));
		free(buf_final(&b.b));
	} else if (ret == Efail) {
		rpt -= 1;
		if (rpt)
			goto again;

		call("Message", ci->focus, 0, NULL,
		     strconcat(ci->focus, "\"", word,
			       "\" is not a word."));
	} else
		call("Message", ci->focus, 0, NULL,
		     strconcat(ci->focus, "Spell check failed for \"", word,
			       "\""));
	mark_free(st);
	return 1;
}

DEF_CMD(emacs_spell_choose)
{
	const char *k = ksuffix(ci, "K:Spell-");
	char match[4] = "( )";
	char *suggest = attr_find(ci->focus->attrs,
				  "spell:suggestions");
	char *last = attr_find(ci->focus->attrs,
			       "spell:last-error");
	char *cp, *ep;
	struct mark *m;
	int i;

	if (!*k || !suggest || !last || !ci->mark)
		return 1;
	match[1] = *k;
	cp = strstr(suggest, match);
	if (!cp)
		return 1;
	cp += 3;
	ep = strchr(cp, ',');
	if (ep)
		cp = strnsave(ci->focus, cp, ep-cp);

	m = mark_dup(ci->mark);
	i = utf8_strlen(last);
	while (i > 0) {
		doc_prev(ci->focus, m);
		i -= 1;
	}
	call("doc:replace", ci->focus, 0, m, cp, 0, ci->mark);
	attr_set_str(&ci->focus->attrs, "spell:suggestions", NULL);
	attr_set_str(&ci->focus->attrs, "spell:last-error", NULL);

	if (ci->num) {
		doc_next(ci->focus, ci->mark);
		home_call(ci->home, "emacs:respell", ci->focus,
			  ci->num, ci->mark, NULL,
			  ci->num2, ci->mark2);
	}

	return 1;
}

DEF_CMD(emacs_spell_abort)
{
	return 1;
}

DEF_CMD(emacs_spell_skip)
{
	if (ci->num) {
		doc_next(ci->focus, ci->mark);
		home_call(ci->home, "emacs:respell", ci->focus,
			  ci->num, ci->mark, NULL,
			  ci->num2, ci->mark2);
	}
	return 1;
}

DEF_CMD(emacs_spell_insert)
{
	char *last = attr_find(ci->focus->attrs,
			       "spell:last-error");
	if (last) {
		attr_set_str(&ci->focus->attrs, "spell:suggestions", NULL);
		attr_set_str(&ci->focus->attrs, "spell:last-error", NULL);
		call("Spell:AddWord", ci->focus, 1, NULL, last);
	}

	if (ci->num) {
		doc_next(ci->focus, ci->mark);
		home_call(ci->home, "emacs:respell", ci->focus,
			  ci->num, ci->mark, NULL,
			  ci->num2, ci->mark2);
	}
	return 1;
}

DEF_CMD(emacs_spell_accept)
{
	char *last = attr_find(ci->focus->attrs,
			       "spell:last-error");
	if (last) {
		attr_set_str(&ci->focus->attrs, "spell:suggestions", NULL);
		attr_set_str(&ci->focus->attrs, "spell:last-error", NULL);
		call("Spell:AddWord", ci->focus, 0, NULL, last);
	}

	if (ci->num) {
		doc_next(ci->focus, ci->mark);
		home_call(ci->home, "emacs:respell", ci->focus,
			  ci->num, ci->mark, NULL,
			  ci->num2, ci->mark2);
	}
	return 1;
}

static int spell_shift(struct pane *focus safe, int num, int inc)
{
	int o = pane_attr_get_int(focus, "spell:offset", 0);
	int i;
	char *msg;
	char *c;

	o += inc;
	msg = pane_attr_get(focus, "spell:suggestions");
	if (!msg)
		return 1;
	for (i = 0; i < o && (c = strchr(msg, ',')) != NULL; i++)
		msg = c+1;
	attr_set_int(&focus->attrs, "spell:offset", i);

	call("Mode:set-all", focus, num, NULL, ":Spell");
	call("Message:modal", focus, 0, NULL, msg);
	return 1;
}

DEF_CMD(emacs_spell_left)
{
	return spell_shift(ci->focus, ci->num, -1);
}

DEF_CMD(emacs_spell_right)
{
	return spell_shift(ci->focus, ci->num, 1);
}

DEF_CMD(emacs_quote)
{
	char b[6];
	wint_t wch = WEOF;
	char *str = NULL;
	struct mark *mk = NULL;
	bool free_mark = False;

	if (ci->num >= 0 && ci->num < NO_NUMERIC)
		wch = ci->num;
	else if (N2(ci) == N2_uniquote && ci->mark &&
		 (str = attr_find(ci->mark->attrs, "emacs:unicode_char")) != NULL) {
		struct call_return cr;
		int i = N2a(ci);
		if (ci->num < 0 && i > 1)
			i -= 1;
		else
			i += 1;
		cr = call_ret(all, "Unicode-names", ci->focus, i, NULL, str);
		if (cr.s && cr.i && (wint_t)cr.i != WEOF) {
			wch = cr.i;
			call("Message", ci->focus, 0, NULL,
			     strconcat(ci->focus,
				       "Unicode char <", cr.s, ">"));
			call("Mode:set-num2", ci->focus,
			     N2_uniquote | (i << 16));
			mk = mark_dup(ci->mark);
			doc_prev(ci->focus, mk);
			free_mark = True;
		} else {
			call("Message", ci->focus, 0, NULL,
			     strconcat(ci->focus,
				       "Cannot find another character <", str, ">"));
			return Efail;
		}
	} else if (wch == WEOF && ci->mark &&
		   (mk = call_ret(mark2, "doc:point", ci->focus)) != NULL &&
		   call("selection:clear", ci->focus, 0, mk) >= 1 &&
		   (str = call_ret(strsave, "doc:get-str", ci->focus,
				   0, NULL, NULL, 0, mk)) != NULL) {
		int x;
		char *ep;
		if (*str == '#')
			str ++;
		x = strtoul(str, &ep, 16);
		if (ep && *ep == 0) {
			wch = x;
			call("Message", ci->focus, 0, NULL,
			     strconcat(ci->focus, "Hex code 0x", str));
		} else {
			struct call_return cr;
			cr = call_ret(all, "Unicode-names", ci->focus,
				      1, NULL, str);
			if (cr.s && cr.i) {
				wch = cr.i;
				call("Message", ci->focus, 0, NULL,
				     strconcat(ci->focus,
					       "Unicode char <", cr.s, ">"));
				if (ci->mark) {
					attr_set_str(&ci->mark->attrs,
						     "emacs:unicode_char",
						     str);
					call("Mode:set-num2", ci->focus,
					     N2_uniquote | (1 << 16));
				}
			} else {
				call("Message", ci->focus, 0, NULL,
				     strconcat(ci->focus,
					       "Cannot find character <", str, ">"));
				return Efail;
			}
		}
	}
	if (wch == WEOF) {
		call("Mode:set-all", ci->focus, ci->num, NULL, ":CQ", ci->num2);
		return 1;
	}
	call("Replace", ci->focus, 0, mk, put_utf8(b, wch));
	if (free_mark)
		mark_free(mk);
	return 1;
}

struct docs_helper {
	struct command c;
	int cnt;
	struct pane *p safe;
};

DEF_CMD(emacs_doc_menu)
{
	const char *d = ksuffix(ci, "emacs:doc-menu:");
	struct pane *p = call_ret(pane, "docs:byname", ci->focus,
				  0, NULL, d);

	if (p) {
		struct pane *t = call_ret(pane, "ThisPane", ci->focus);
		if (t)
			home_call(p, "doc:attach-view", t, 1);
	}
	return 1;
}

DEF_CB(emacs_menu_add_doc)
{
	struct docs_helper *dh = container_of(ci->comm, struct docs_helper, c);
	char *name = pane_attr_get(ci->focus, "doc-name");

	if (!name)
		return Efallthrough;
	if (dh->cnt >= 10)
		return 1;
	dh->cnt += 1;
	call("menu:add", dh->p, 0, NULL, name, 0, NULL,
	     strconcat(ci->home, " emacs:doc-menu:", name));
	return Efallthrough;
}

DEF_CMD(emacs_menu_refresh)
{
	struct pane *p = ci->focus;
	char *n = pane_attr_get(p, "doc-name");
	struct docs_helper dh;

	if (!n || strcmp(n, "Documents") != 0)
		return 1;

	call("menu:clear", p);
	dh.c = emacs_menu_add_doc;
	dh.cnt = 0;
	dh.p = p;
	call_comm("docs:byeach", p, &dh.c);
	call("menu:add", p, 0, NULL, "List all", 0, NULL, ":C-X :C-B");
	return 1;
}

static char *menus[][3] = {
	{ "Help/Recent", ":F1 l", "R" },
	{ "Documents/List all", ":C-X :C-B", "R"},
	{ "File/Open", ":C-X :C-F", "L"},
	{ "File/Save", ":C-X :C-S", "L"},
	{ "File/Exit", ":C-X :C-C", "L"},
	{ "Edit/Copy", ":A-w", "L"},
};

DEF_CMD(emacs_menubar_configure)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(menus); i++)
		call("menubar-add", ci->focus,
		     menus[i][2][0] == 'R' ? 2 : 0,
		     NULL, menus[i][0],
		     0, NULL, menus[i][1]);
	/* Allow other ancestor to configure */
	return Efallthrough;
}

DEF_PFX_CMD(cx_cmd, ":CX");
DEF_PFX_CMD(cx4_cmd, ":CX4");
DEF_PFX_CMD(cx5_cmd, ":CX5");
DEF_PFX_CMD(cx44_cmd, ":CX44");
DEF_PFX_CMD(cc_cmd, ":CC");

static struct map *emacs_map;
DEF_LOOKUP_CMD(mode_emacs, emacs_map);

static void emacs_init(void)
{
	unsigned i;
	struct map *m;

	if (emacs_map)
		return;
	m = key_alloc();
	key_add(m, "K:C-X", &cx_cmd.c);
	key_add(m, "K:CX-4", &cx4_cmd.c);
	/* C-\ is generated by C-4.  Weird... */
	key_add(m, "K:CX:C-\\", &cx4_cmd.c);
	key_add(m, "K:CX-5", &cx5_cmd.c);
	key_add(m, "K:CX4-4", &cx44_cmd.c);
	key_add(m, "K:CX4:C-\\", &cx44_cmd.c);
	key_add(m, "K:C-C", &cc_cmd.c);

	key_add(m, "K:C-Q", &emacs_quote);

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

	key_add(m, "K:C-O", &emacs_open_line);

	key_add(m, "K:C-_", &emacs_undo);
	key_add(m, "K:CX-u", &emacs_undo);
	key_add(m, "K:C-/", &emacs_undo);
	key_add(m, "K:C-Z", &emacs_undo);

	key_add(m, "K:C-L", &emacs_recenter);

	key_add(m, "K:CX:C-F", &emacs_findfile);
	key_add(m, "K:CX4:C-F", &emacs_findfile);
	key_add(m, "K:CX4-f", &emacs_findfile);
	key_add(m, "K:CX44-f", &emacs_findfile);
	key_add_prefix(m, "File Found:", &emacs_findfile);

	key_add(m, "K:CX:C-W", &emacs_writefile);
	key_add_prefix(m, "emacs:write_file:", &emacs_do_writefile);

	key_add(m, "K:CX-i", &emacs_insertfile);
	key_add_prefix(m, "emacs:insert_file:", &emacs_do_insertfile);

	key_add(m, "K:CX-b", &emacs_finddoc);
	key_add(m, "K:CX4-b", &emacs_finddoc);
	key_add(m, "K:CX44-b", &emacs_finddoc);
	key_add_prefix(m, "Doc Found:", &emacs_finddoc);

	key_add(m, "K:CX:C-B", &emacs_viewdocs);
	key_add(m, "K:CX4:C-B", &emacs_viewdocs);
	key_add(m, "K:CX44:C-B", &emacs_viewdocs);

	key_add(m, "K:CX-k", &emacs_kill_doc);

	key_add(m, "K:CX-s", &emacs_save_all);

	key_add(m, "K:CX:C-V", &emacs_revisit);

	key_add(m, "K:CX-=", &emacs_curs_pos);
	key_add(m, "K:A-=", &emacs_word_count);

	key_add(m, "K:CX-<", &emacs_shift);
	key_add(m, "K:CX->", &emacs_shift);
	key_add(m, "K-<", &emacs_shift_again);
	key_add(m, "K->", &emacs_shift_again);

	key_add(m, "K:CX-{", &emacs_growx);
	key_add(m, "K:CX-}", &emacs_growx);
	key_add(m, "K-{", &emacs_growx_again);
	key_add(m, "K-}", &emacs_growx_again);

	key_add(m, "K:CX:C-=", &emacs_scale_relative);
	key_add(m, "K:CX:C--", &emacs_scale_relative);

	key_add(m, "K:C-S", &emacs_start_search);
	key_add(m, "K:C-R", &emacs_start_search);
	key_add(m, "K:A-%", &emacs_start_search);
	key_add(m, "render:reposition", &emacs_reposition);

	key_add(m, "K:CX:C-C", &emacs_exit);
	key_add(m, "emacs:deactivate", &emacs_deactivate);

	key_add(m, "K:C-U", &emacs_prefix);

	key_add(m, "K:A-!", &emacs_shell);
	key_add(m, "K:A-|", &emacs_shell);
	key_add(m, "Shell Command", &emacs_shell);

	key_add(m, "K:CX-`", &emacs_next_match);
	key_add(m, "K-`", &emacs_match_again);

	key_add(m, "K:CX-1", &emacs_close_others);
	key_add(m, "K-1", &emacs_close_others);

	key_add_range(m, "K:A-0", "K:A-9", &emacs_num);
	key_add(m, "K:A--", &emacs_neg);
	key_add(m, "K:C--", &emacs_neg);
	key_add(m, "K:C- ", &emacs_mark);
	key_add(m, "mode-set-mark", &emacs_mark);
	key_add(m, "mode-swap-mark", &emacs_swap_mark);
	key_add(m, "K:C-W", &emacs_wipe);
	key_add(m, "K:A-w", &emacs_copy);
	key_add(m, "K:C-Y", &emacs_yank);
	key_add(m, "K:A-y", &emacs_yank_pop);

	key_add(m, "K:A-g", &emacs_goto_line);
	key_add(m, "K:A-x", &emacs_command);
	key_add(m, "K:A-X", &emacs_command);
	key_add(m, "K:CC-m", &emacs_make);
	key_add(m, "K:CC:C-M", &emacs_make);

	key_add(m, "K:A:C-V", &emacs_move_view_other);

	key_add(m, "K:CX:C-Q", &emacs_readonly);

	key_add_prefix(m, "K:CQ-", &emacs_quote_insert);
	key_add_prefix(m, "K:CQ:C-", &emacs_quote_insert);

	key_add(m, "K:A-q", &emacs_fill);
	key_add(m, "K:A:C-Q", &emacs_fill);
	key_add(m, "K:A-/", &emacs_abbrev);
	key_add(m, "K:A-;", &emacs_spell);
	key_add(m, "emacs:respell", &emacs_spell);
	key_add_prefix(m, "K:Spell-", &emacs_spell_choose);
	key_add(m, "K:Spell-a", &emacs_spell_accept);
	key_add(m, "K:Spell-i", &emacs_spell_insert);
	key_add(m, "K:Spell- ", &emacs_spell_skip);
	key_add(m, "K:Spell:Enter", &emacs_spell_abort);
	key_add(m, "K:Spell:ESC", &emacs_spell_abort);
	key_add(m, "K:Spell:Left", &emacs_spell_left);
	key_add(m, "K:Spell:C-B", &emacs_spell_left);
	key_add(m, "K:Spell:Right", &emacs_spell_right);
	key_add(m, "K:Spell:C-F", &emacs_spell_right);

	key_add(m, "K:Help-l", &emacs_showinput);

	key_add(m, "emacs:command", &emacs_do_command);
	key_add(m, "interactive-cmd-version", &emacs_version);
	key_add(m, "interactive-cmd-log", &emacs_log);

	key_add(m, "M:Click-2", &emacs_paste);
	key_add(m, "M:C:Click-1", &emacs_paste);

	key_add(m, "K:CX-(", &emacs_macro_start);
	key_add(m, "K:CX-)", &emacs_macro_stop);
	key_add(m, "K:CX-e", &emacs_macro_run);
	key_add(m, "K-e", &emacs_macro_run);

	key_add(m, "menu:refresh", &emacs_menu_refresh);
	key_add_prefix(m, "emacs:doc-menu:", &emacs_doc_menu);

	key_add(m, "menubar:ready", &emacs_menubar_configure);

	emacs_map = m;
}

DEF_CMD(attach_mode_emacs)
{
	struct pane *p = pane_register(ci->focus, 0, &mode_emacs.c, NULL);

	if (!p)
		return Efail;
	comm_call(ci->comm2, "cb", p);
	return 1;
}

DEF_CMD(attach_file_entry)
{
	/* The 'type' passed must be static, not allocated */
	char *type = "shellcmd";

	if (ci->str && strcmp(ci->str, "file") == 0)
		type = "file";
	else if (ci->str && strcmp(ci->str, "doc") == 0)
		type = "doc";
	pane_register(ci->focus, 0, &find_handle.c, type);

	return 1;
}

void edlib_init(struct pane *ed safe)
{
	emacs_init();
	findmap_init();
	call_comm("global-set-command", ed, &attach_mode_emacs, 0, NULL, "attach-mode-emacs");
	call_comm("global-set-command", ed, &attach_file_entry, 0, NULL, "attach-file-entry");
	call_comm("global-set-command", ed, &emacs_shell, 0, NULL, "attach-shell-prompt");
	call_comm("global-set-command", ed, &emacs_selection_menu_add,
		  0, NULL, "selection-menu:add-00-emacs");
}
