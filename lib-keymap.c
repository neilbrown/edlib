/*
 * Copyright Neil Brown ©2015 <neil@brown.name>
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
	struct map	*map safe;
	struct command	**cmds;
	struct command	*globalcmd;
	int		cmdcount;
	int		global;
};

static struct pane *do_keymap_attach(struct pane *p, int global) safe;

DEF_CMD(get_command_callback)
{
	struct call_return *cr = container_of(ci->comm, struct call_return , c);
	cr->comm = ci->comm2;
	return 1;
}

static struct command *get_command(struct pane *p safe, char *cmd)
{
	struct call_return cr = {};
	int ret;

	cr.c = get_command_callback;
	ret = call_comm("global-get-command", p, 0, NULL, cmd, 0, &cr.c);
	if (ret > 0)
		return cr.comm;
	return NULL;
}

DEF_CMD(keymap_handle)
{
	struct key_data *kd = ci->home->data;
	int i;

	if (strcmp(ci->key, "Close") == 0) {
		command_put(kd->globalcmd);
		key_free(kd->map);
		if (kd->cmds != &kd->globalcmd) {
			for (i = 0; i < kd->cmdcount; i++)
				command_put(kd->cmds[i]);
			free(kd->cmds);
		}
		free(kd);
		return 1;
	}
	if (strcmp(ci->key, "Clone") == 0) {
		struct pane *p = do_keymap_attach(ci->focus, kd->global);
		struct key_data *kd_old = ci->home->data;
		struct key_data *kd_new;
		if (!p)
			return -1;
		kd_new = p->data;
		kd_new->globalcmd = command_get(kd_old->globalcmd);
		if (kd_old->cmds == &kd_old->globalcmd) {
			kd_new->cmds = &kd_new->globalcmd;
			kd_new->cmdcount = 1;
		} else if (kd_old->cmds) {
			kd_new->cmdcount = kd_old->cmdcount;
			kd_new->cmds = malloc(kd_new->cmdcount * sizeof(kd_new->cmds[0]));
			for (i = 0; i < kd_new->cmdcount; i++)
				kd_new->cmds[i] = command_get(kd_old->cmds[i]);
		}
		pane_clone_children(ci->home, p);
		return 1;
	}

	if (kd->global && strncmp(ci->key, "local-", 6) == 0) {
		if (strcmp(ci->key, "local-set-key") == 0 ||
		    strcmp(ci->key, "local-add-keymap") == 0 ||
		    strcmp(ci->key, "local-remove-keymap") == 0) {
			/* Add a local keymap on 'focus' and re-send */
			return call7(ci->key, do_keymap_attach(ci->focus, 0),
				     ci->numeric, ci->mark, ci->str, ci->extra,
				     ci->str2, ci->mark2);
		}
	}
	if (kd->global && strncmp(ci->key, "global-set-key", 14) == 0) {
		if (strcmp(ci->key, "global-set-key") == 0) {
		}
		if (strcmp(ci->key, "global-set-keymap") == 0) {
			struct command *cm = ci->comm2;
			if (!cm && ci->str)
				cm = get_command(ci->home, ci->str);
			if (!cm)
				return -1;
			kd->globalcmd = command_get(cm);
			kd->cmds = &kd->globalcmd;
			kd->cmdcount = 1;
			return 1;
		}
	}
	if (!kd->global && strncmp(ci->key, "local-", 6) == 0) {
		if (strcmp(ci->key, "local-add-keymap") == 0) {
			struct command *cm = get_command(ci->home, ci->str);
			if (!cm)
				return -1;
			kd->cmds = realloc(kd->cmds, ((kd->cmdcount+1)*
						      sizeof(kd->cmds[0])));
			kd->cmds[kd->cmdcount++] = command_get(cm);
			return 1;
		}
		if (strcmp(ci->key, "local-set-key") == 0) {
			struct command *cm = get_command(ci->home, ci->str);
			if (!cm)
				return -1;
			key_add(kd->map, ci->str2, cm);
			return 1;
		}
	}

	for (i = 0; i < kd->cmdcount; i++) {
		int ret;
		((struct cmd_info*)ci)->comm = kd->cmds[i];
		ret = kd->cmds[i]->func(ci);
		if (ret)
			return ret;
	}

	return key_lookup(kd->map, ci);
}

static struct pane *do_keymap_attach(struct pane *p, int global) safe
{
	struct key_data *kd = malloc(sizeof(*kd));

	kd->map = key_alloc();
	kd->cmds = NULL;
	kd->globalcmd = NULL;
	kd->cmdcount = 0;
	kd->global = global;
	return pane_register(p, 0, &keymap_handle, kd, NULL);
}

DEF_CMD(keymap_attach)
{
	struct pane *p = do_keymap_attach(ci->focus, 1);
	if (p)
		return comm_call(ci->comm2, "callback:attach", p, 0, NULL, NULL, 0);
	return -1;
}

void edlib_init(struct pane *ed safe)
{
	call_comm("global-set-command", ed, 0, NULL, "attach-global-keymap",
		  0, &keymap_attach);
}
