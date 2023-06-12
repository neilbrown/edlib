/*
 * Copyright Neil Brown ©2020-2023 <neil@brown.name>
 * May be distributed under terms of GPLv2 - see file:COPYING
 *
 * Individual document handlers are responsible for creating
 * autosave files. The task of this module is to provide access
 * to those files.
 *
 * - When a file is visited we check if it has an autosaved version.
 *   If so, we display a diff and ask if it should be restored.
 */

#include <unistd.h>
#include <dirent.h>
#include "core.h"

/*
 * Autosave restore:
 * When an old autosave file is detected, we pop up a window showing a diff
 * from the saved file to the autosave file and as 'Should this be restored?'
 * If 'y' or 's' is given, the autosave file is renamed over the saved file,
 * which is then reloaded.
 * 'n' or 'q' discard the diff.
 */

static const char mesg[] =
	"\nAutosave file has these differences, type:\n"
	"'y' to restore,\n"
	"'n' to ignore,\n"
	"'d' to delete autosaved file.\n\n";

DEF_CMD(autosave_keep)
{
	char *orig = pane_attr_get(ci->focus, "orig_name");
	char *as = pane_attr_get(ci->focus, "autosave_name");
	struct pane *d;

	if (!orig || !as)
		return 1;

	d = call_ret(pane, "doc:open", ci->focus, -1, NULL, orig);
	if (d)
		call("doc:load-file", d, 4, NULL, NULL, -1);
	call("popup:close", ci->focus);
	return 1;
}

DEF_CMD(autosave_ignore)
{
	call("popup:close", ci->focus);
	return 1;
}

DEF_CMD(autosave_del)
{
	char *fn = pane_attr_get(ci->focus, "orig_name");
	char *as = pane_attr_get(ci->focus, "autosave_name");
	char *ast = pane_attr_get(ci->focus, "autosave_type");
	struct pane *d;

	if (!fn || !as || !ast)
		return Efail;

	/* 4 is autocreate */
	d = call_ret(pane, "doc:open", ci->focus, -1, NULL, fn, 4);
	if (!d) {
		call("Message", ci->focus, 0, NULL,
		     strconcat(ci->focus, "Cannot open ", fn));
		return Efail;
	}
	if (strcmp(ast, "autosave") == 0) {
		if (call("doc:autosave-delete", d, 0, NULL, as) == 1)
			call("Message", ci->focus, 0, NULL,
			     strconcat(ci->focus, as, " deleted."));
	} else {
		if (unlink(as) == 0)
			call("Message", ci->focus, 0, NULL,
			     strconcat(ci->focus, as, " deleted."));
	}
	call("popup:close", ci->focus);
	return 1;
}

DEF_CMD(autosave_dir_view)
{
	/* Open in other pane, and follow symlink */
	home_call(ci->home->parent, "doc:cmd-o", ci->focus, 1);
	return 2;
}

DEF_CMD(autosave_dir_ignore)
{
	struct mark *m;

	/* If this is the last, then bury the doc */
	if (!ci->mark)
		return Enoarg;
	m = mark_dup(ci->mark);
	doc_next(ci->home->parent, m);
	if (call("doc:render-line", ci->focus, 0, m) < 0 ||
	    m->ref.p == NULL)
		call("Window:bury", ci->focus);
	mark_free(m);
	/* Ask viewer to move forward */
	return 2;
}

DEF_CMD(autosave_dir_delete)
{
	struct mark *m = ci->mark;
	char *dir;
	char *base;
	char *fn;

	if (!m)
		return Enoarg;
	fn = pane_mark_attr(ci->focus, m, "target");
	if (!fn || *fn != '/')
		return 2;

	dir = pane_attr_get(ci->focus, "dirname");
	base = pane_mark_attr(ci->focus, m, "name");
	if (dir && base) {
		fn = strconcat(ci->focus, dir, base);
		unlink(fn);
		/* trigger a directory reread */
		call("doc:notify:doc:revisit", ci->focus, 1);
		return 1;
	} else
		/* Move to next */
		return 2;
}

DEF_CMD(autosave_dir_empty)
{
	call("Window:bury", ci->focus);
	return 1;
}

static struct map *asd_map;
DEF_LOOKUP_CMD(autosavedir_handle, asd_map);

static struct map *as_map;
DEF_LOOKUP_CMD(autosave_handle, as_map);
static void autosave_init(void)
{
	if (as_map)
		return;
	as_map = key_alloc();
	key_add(as_map, "doc:cmd-s", &autosave_keep);
	key_add(as_map, "doc:cmd-y", &autosave_keep);
	key_add(as_map, "doc:cmd-d", &autosave_del);
	key_add(as_map, "doc:cmd-n", &autosave_ignore);
	key_add(as_map, "doc:cmd-q", &autosave_ignore);
	key_add(as_map, "doc:replaced", &autosave_ignore);

	asd_map = key_alloc();
	key_add(asd_map, "doc:cmd-v", &autosave_dir_view);
	key_add(asd_map, "doc:cmd-y", &autosave_dir_view);
	key_add(asd_map, "doc:cmd-f", &autosave_dir_view);
	key_add(asd_map, "doc:cmd-o", &autosave_dir_view);
	key_add(asd_map, "doc:cmd-\n", &autosave_dir_view);
	key_add(asd_map, "doc:cmd:Enter", &autosave_dir_view);

	key_add(asd_map, "doc:cmd-d", &autosave_dir_delete);
	key_add(asd_map, "doc:cmd-i", &autosave_dir_ignore);
	key_add(asd_map, "doc:cmd-n", &autosave_dir_ignore);
	key_add(asd_map, "Notify:filter:empty", &autosave_dir_empty);

}

DEF_CB(choose_new)
{
	struct call_return *cr = container_of(ci->comm, struct call_return, c);

	if (cr->p == NULL || ci->num > cr->i) {
		cr->p = ci->focus;
		cr->i = ci->num;
	}
	return 1;
}

DEF_CMD(ask_autosave)
{
	struct pane *p = ci->focus;
	struct pane *p2;
	struct call_return cr;
	char *f = NULL, *a = NULL, *diffcmd;
	char *s;
	struct pane *doc;
	char *autosave_type = "";

	/* Need to choose best display */
	cr.i = 0; cr.p = NULL;
	cr.c = choose_new;
	call_comm("editor:notify:all-displays", p, &cr.c);
	if (!cr.p)
		/* No display!!! */
		return Efail;

	p2 = call_ret(pane, "PopupTile", pane_leaf(cr.p), 0, NULL, "DM3sta");
	if (!p2)
		return Efail;

	if ((s = pane_attr_get(p, "autosave-exists")) != NULL &&
	    strcmp(s, "yes") == 0) {
		f = pane_attr_get(p, "filename");
		a = pane_attr_get(p, "autosave-name");
		autosave_type = "autosave";
	} else if ((s = pane_attr_get(p, "is_backup")) != NULL &&
		   strcmp(s, "yes") == 0) {
		f = pane_attr_get(p, "base-name");
		a = pane_attr_get(p, "filename");
		autosave_type = "backup";
	}

	if (!a || !f) {
		call("popup:close", p2);
		return Efail;
	}
	doc = call_ret(pane, "doc:from-text", p,
		       0, NULL, "*Autosave-Diff*",
		       0, NULL, mesg);
	if (doc) {
		call("doc:replace", doc, 0, NULL, "Original file: ");
		call("doc:replace", doc, 0, NULL, f);
		call("doc:replace", doc, 0, NULL, "\nAutosave file: ");
		call("doc:replace", doc, 0, NULL, a);
		call("doc:replace", doc, 0, NULL, "\n\n");
		call("doc:set:autoclose", doc, 1);
		diffcmd = strconcat(p, "diff -Nu ",f," ",a);
		call("attach-shellcmd", doc, 2, NULL, diffcmd);
		attr_set_str(&doc->attrs, "view-default", "diff");
		p2 = home_call_ret(pane, doc, "doc:attach-view", p2, 1);
	} else
		p2 = NULL;
	if (p2)
		p2 = pane_register(p2, 0, &autosave_handle.c);
	if (p2) {
		attr_set_str(&p2->attrs, "orig_name", f);
		attr_set_str(&p2->attrs, "autosave_name", a);
		attr_set_str(&p2->attrs, "autosave_type", autosave_type);
		if (doc)
			pane_add_notify(p2, doc, "doc:replaced");
	}

	return 1;
}

DEF_CMD(check_autosave)
{
	char *s;
	struct pane *p = ci->focus;

	s = pane_attr_get(p, "filename");
	if (s && strlen(s) > 17 &&
	    strcmp(s + strlen(s) - 17, "/.edlib_autosave/") == 0) {
		attr_set_str(&p->attrs, "view-default", "autosave-dir-view");
	}

	s = pane_attr_get(p, "autosave-exists");
	if (!s || strcmp(s, "yes") != 0)
		s = pane_attr_get(p, "is_backup");
	if (s && strcmp(s, "yes") == 0)
		call_comm("event:on-idle", p, &ask_autosave);

	return Efallthrough;
}

DEF_CMD(attach_asview)
{
	struct pane *p;

	p = call_ret(pane, "attach-dirview", ci->focus);
	if (!p)
		return Efail;
	p = pane_register(p, 0, &autosavedir_handle.c);
	if (!p)
		return Efail;
	attr_set_str(&p->attrs, "line-format", " %target");
	attr_set_str(&p->attrs, "heading",
		     "Autosave files: [v]iew, [d]elete, [i]gnore");
	p = call_ret(pane, "attach-linefilter", p);
	if (p) {
		attr_set_str(&p->attrs, "filter:attr", "arrow");
		attr_set_str(&p->attrs, "filter:match", " -> ");
		comm_call(ci->comm2, "cb", p);
	}
	return 1;
}

DEF_CMD(show_autosave)
{
	struct pane *p, *d;
	char *home = getenv("HOME");
	char *dirname = getenv("EDLIB_AUTOSAVE");

	if (!dirname) {
		if (!home) {
			call("Message", ci->focus, 0, NULL,
			     "Cannot determine HOME directory");
			return 1;
		}
		dirname = strconcat(ci->focus, home, "/.edlib_autosave");
	}
	p = call_ret(pane, "ThisPane", ci->focus);
	if (!p)
		return Efail;
	d = call_ret(pane, "doc:open", p, -1, NULL, dirname);
	if (d)
		home_call_ret(pane, d, "doc:attach-view", p,
			      0, NULL, "simple");
	else {
		call("Message", ci->focus, 0, NULL,
		     "Cannot open $HOME/.edlib_autosave");
		pane_close(p);
	}
	return 1;
}

DEF_CMD(check_autosave_dir)
{
	/* Should I open the direct doc and use filter to do the search?? */
	DIR *dir;
	struct dirent *de;
	char *home = getenv("HOME");
	char *dirname = getenv("EDLIB_AUTOSAVE");

	if (!dirname)
		dirname = strconcat(ci->focus, home ?: "", "/.edlib_autosave");
	dir = opendir(dirname);
	if (!dir)
		return 1;
	while ((de = readdir(dir)) != NULL) {
		if (de->d_name[0] == '.')
			continue;
		if (de->d_type == DT_LNK)
			break;
		if (de->d_type != DT_UNKNOWN)
			continue;
		/* FIXME I should probably lstat the name */
		continue;
	}
	closedir(dir);
	if (de)
		call("editor:notify:Message:broadcast", ci->focus, 0, NULL,
		     "Autosave files exist - use \"recover\" command to view them.");
	return 1;
}

void edlib_init(struct pane *ed safe)
{
	autosave_init();
	call_comm("global-set-command", ed, &check_autosave,
		  0, NULL, "doc:appeared-check-autosave");
	call_comm("global-set-command", ed, &attach_asview,
		  0, NULL, "attach-autosave-dir-view");
	call_comm("global-set-command", ed, &show_autosave,
		  0, NULL, "interactive-cmd-recover");
	call_comm("global-set-command", ed, &check_autosave_dir,
		  0, NULL, "startup-autosave");

}
