/*
 * Copyright Neil Brown ©2015-2021 <neil@brown.name>
 * May be distributed under terms of GPLv2 - see file:COPYING
 *
 * Keymap management panes for edlib.
 *
 * A keymap pane makes it easy to attach keymaps into a pane tree.
 *   global-set-keymap
 * is given a command which is used to as all incoming requests.
 */

#include <stdlib.h>
#include <string.h>
#include "core.h"

struct key_data {
	struct command	*globalcmd;
};

static struct pane *safe do_keymap_attach(struct pane *p);

DEF_CMD(keymap_handle)
{
	struct key_data *kd = ci->home->data;

	if (strcmp(ci->key, "Close") == 0) {
		command_put(kd->globalcmd);
		return 1;
	}
	if (strcmp(ci->key, "Free") == 0) {
		free(kd);
		ci->home->data = safe_cast NULL;
		return 1;
	}
	if (strcmp(ci->key, "Clone") == 0) {
		struct pane *p = do_keymap_attach(ci->focus);
		struct key_data *kd_old = ci->home->data;
		struct key_data *kd_new;
		if (!p)
			return Efail;
		kd_new = p->data;
		if (kd_old->globalcmd)
			kd_new->globalcmd = command_get(kd_old->globalcmd);

		pane_clone_children(ci->home, p);
		return 1;
	}

	if (kd->globalcmd) {
		int ret;
		((struct cmd_info*)ci)->comm = kd->globalcmd;
		ret = kd->globalcmd->func(ci);
		if (ret)
			return ret;
	}
	if (strcmp(ci->key, "global-set-keymap") == 0) {
		struct command *cm = ci->comm2;
		if (!cm)
			return Enoarg;
		command_put(kd->globalcmd);
		kd->globalcmd = command_get(cm);
		return 1;
	}

	return Efallthrough;
}

static struct pane *safe do_keymap_attach(struct pane *p)
{
	struct key_data *kd = malloc(sizeof(*kd));

	kd->globalcmd = NULL;
	return pane_register(p, 0, &keymap_handle, kd);
}

DEF_CMD(keymap_attach)
{
	struct pane *p = do_keymap_attach(ci->focus);
	if (p)
		return comm_call(ci->comm2, "callback:attach", p);
	return Efail;
}

void edlib_init(struct pane *ed safe)
{
	call_comm("global-set-command", ed, &keymap_attach, 0, NULL,
		  "attach-global-keymap");
}
