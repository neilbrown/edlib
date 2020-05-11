/*
 * Copyright Neil Brown ©2020 <neil@brown.name>
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
#include "core.h"

/*
 * Autosave restore:
 * When an old autosave file is detected, we pop up a window showing a diff
 * from the saved file to the autosave file and as 'Should this be restored?'
 * If 'y' or 's' is given, the autosave file is renamed over the saved file,
 * which is then reloaded.
 * 'n' or 'q' discard the diff.
 */

static char mesg[] =
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
	char *as = pane_attr_get(ci->focus, "autosave_name");

	if (!as)
		return 1;
	unlink(as);
	call("Message", ci->focus, 0, NULL,
	     strconcat(ci->focus, as, " deleted."));
	call("popup:close", ci->focus);
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
}

DEF_CMD(choose_new)
{
	struct call_return *cr = container_of(ci->comm, struct call_return, c);

	if (cr->p == NULL || ci->num > cr->i) {
		cr->p = ci->focus;
		cr->i = ci->num;
	}
	return 0;
}

DEF_CMD(ask_autosave)
{
	struct pane *p = ci->focus;
	struct pane *p2;
	struct call_return cr;
	char *f = NULL, *a = NULL, *diffcmd;
	char *s;
	struct pane *doc;

	/* Need to choose best display */
	cr.i = 0; cr.p = NULL;
	cr.c = choose_new;
	call_comm("editor:notify:all-displays", p, &cr.c);
	if (!cr.p)
		/* No display!!! */
		return Efail;
	while (cr.p->focus)
		cr.p = cr.p->focus;

	p2 = call_ret(pane, "PopupTile", cr.p, 0, NULL, "DM3sta");
	if (!p2)
		return Efail;

	if ((s = pane_attr_get(p, "autosave-exists")) != NULL &&
	    strcmp(s, "yes") == 0) {
		f = pane_attr_get(p, "filename");
		a = pane_attr_get(p, "autosave-name");
	} else if ((s = pane_attr_get(p, "is_backup")) != NULL &&
		   strcmp(s, "yes") == 0) {
		f = pane_attr_get(p, "base-name");
		a = pane_attr_get(p, "filename");
	}

	if (!a || !f) {
		pane_close(cr.p);
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
		call("attach-shellcmd", doc, 1, NULL, diffcmd);
		attr_set_str(&doc->attrs, "view-default", "diff");
		p2 = home_call_ret(pane, doc, "doc:attach-view", p2, 1);
	} else
		p2 = NULL;
	if (p2)
		p2 = pane_register(p2, 0, &autosave_handle.c);
	if (p2) {
		attr_set_str(&p2->attrs, "orig_name", f);
		attr_set_str(&p2->attrs, "autosave_name", a);
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
		call_comm("editor-on-idle", p, &ask_autosave);

	return Efallthrough;
}

DEF_CMD(attach_asview)
{
	struct pane *p;

	p = call_ret(pane, "attach-render-format", ci->focus);
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


void edlib_init(struct pane *ed safe)
{
	autosave_init();
	call_comm("global-set-command", ed, &check_autosave,
		  0, NULL, "doc:appeared-check-autosave");
	call_comm("global-set-command", ed, &attach_asview,
		  0, NULL, "attach-autosave-dir-view");
}
