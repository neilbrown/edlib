/*
 * Copyright Neil Brown <neil@brown.name> 2015
 * May be distributed under terms of GPLv2 - see file:COPYING
 *
 * Keymap management panes for edlib.
 *
 * A keymap pane makes it easy to attach keymaps into a pane tree.
 * Each keymap pane is designated 'global' or 'local'.
 * Global panes respond to:
 *   global-set-key global-set-keymap
 * Local panes respond to
 *   local-set-key local-add-keymap local-remove-keymap
 *
 * If a global pane gets a local command, it attaches a local keymap
 * at the focus and resubmits the request.
 *
 * Each pane has one or (for local panes) more keymap commands and
 * one writable keymap (initially empty).
 * A keymap command will typically perform a lookup in a private keymap.
 *
 * *set-key commands will receive the keyname and the command name in
 * the two strings: str and str2.  We lookup str2 in doc->something.
 */

#include <stdlib.h>
#include <string.h>
#include "core.h"

struct key_data {
	struct map	*map;
	struct command	**cmds, *globalcmd;
	int		cmdcount;
	int		global;
};

static int keymap_attach_func(struct cmd_info *ci);

DEF_CMD(keymap_handle)
{
	struct key_data *kd = ci->home->data;
	int i;

	if (strcmp(ci->key, "Close") == 0) {
		key_free(kd->map);
		if (kd->cmds != &kd->globalcmd)
			free(kd->cmds);
		free(kd);
		return 1;
	}
	if (strcmp(ci->key, "Clone") == 0) {
		//struct pane *p2;
		//	p2 = key_attach(ci->focus, ci->home->map);
		//if (ci->focus->focus)
		//	return pane_clone(ci->focus->focus, p2);
		return 1;
	}

	if (kd->global && strcmp(ci->key, "global-key-root") == 0) {
		ci->focus = ci->home;
		return 1;
	}
	if (kd->global && strncmp(ci->key, "local-", 6) == 0) {
		if (strcmp(ci->key, "local-set-key") == 0 ||
		    strcmp(ci->key, "local-add-keymap") == 0 ||
		    strcmp(ci->key, "local-remove-keymap") == 0) {
			/* Add a local keymap on 'focus' and re-send */
			struct pane *p = ci->focus;
			struct cmd_info ci2 = {0};
			ci2.focus = p;
			keymap_attach_func(&ci2);
			pane_attach(p, "local-keymap", NULL, NULL);
			return key_handle_focus(ci);
		}
	}
	if (kd->global && strncmp(ci->key, "global-set-key", 14) == 0) {
		if (strcmp(ci->key, "global-set-key") == 0) {
		}
		if (strcmp(ci->key, "global-set-keymap") == 0) {
			struct editor *ed = pane2ed(ci->home);
			struct command *cm = key_lookup_cmd(ed->commands, ci->str);
			if (!cm)
				return -1;
			kd->globalcmd = cm;
			kd->cmds = &kd->globalcmd;
			kd->cmdcount = 1;
			return 1;
		}
	}
	if (!kd->global && strncmp(ci->key, "local-", 6) == 0) {
		if (strcmp(ci->key, "local-add-keymap") == 0) {
			struct editor *ed = pane2ed(ci->home);
			struct command *cm = key_lookup_cmd(ed->commands, ci->str);
			if (!cm)
				return -1;
			kd->cmds = realloc(kd->cmds, ((kd->cmdcount+1)*
						      sizeof(kd->cmds[0])));
			kd->cmds[kd->cmdcount++] = cm;
			return 1;
		}
		if (strcmp(ci->key, "local-set-key") == 0) {
			struct editor *ed = pane2ed(ci->home);
			struct command *cm = key_lookup_cmd(ed->commands, ci->str);

			if (!cm)
				return -1;
			key_add(kd->map, ci->str2, cm);
			return 1;
		}
	}

	for (i = 0; i < kd->cmdcount; i++) {
		int ret;
		ci->comm = kd->cmds[i];
		ret = kd->cmds[i]->func(ci);
		if (ret)
			return ret;
	}

	return key_lookup(kd->map, ci);
}

DEF_CMD(keymap_attach)
{
	struct key_data *kd = malloc(sizeof(*kd));
	struct pane *p;

	kd->map = key_alloc();
	kd->cmds = NULL;
	kd->globalcmd = NULL;
	kd->cmdcount = 0;
	kd->global = ci->comm ? 1 : 0;
	p = ci->focus;
	while (pane_child(p))
		p = pane_child(p);
	p = pane_register(p, 0, &keymap_handle, kd, NULL);
	pane_check_size(p);
	ci->focus = p;
	return 1;
}

void edlib_init(struct editor *ed)
{
	key_add(ed->commands, "attach-global-keymap", &keymap_attach);
}
