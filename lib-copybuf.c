/*
 * Copyright Neil Brown ©2017-2022 <neil@brown.name>
 * May be distributed under terms of GPLv2 - see file:COPYING
 *
 * copybuf
 *
 * A copy-buffer stores a number of texts that have been copied from
 * elsewhere.  It would be nice to store these in a text document, but
 * as undo cannot be disabled, that would not be good for now.
 * So lets just have a linked list of things.
 *
 * New texts can be added, old texts (indexed from most recent: 0 is latest, 1 is second
 * latest) can be requested.
 * Never store more than 10 texts.
 *
 * Register global commands "copy:save" and "copy:get" to access texts.
 */

#include <unistd.h>
#include <stdlib.h>
#include <string.h>

#define PANE_DATA_TYPE struct copy_info
#include "core.h"

struct copy_info {
	struct txt {
		struct txt *next;
		char	*txt safe;
	} *store;
	int		count;
	struct command	cmd;
	struct pane	*pane;
};
#include "core-pane.h"

static struct map *copy_map;
DEF_LOOKUP_CMD(copy_handle, copy_map);

static void free_txt(struct txt **tp safe)
{
	struct txt *t = *tp;
	if (!t)
		return;
	*tp = NULL;
	free(t->txt);
	free_txt(&t->next);
	free(t);
}

DEF_CMD_CLOSED(copy_close)
{
	struct copy_info *cyi = ci->home->data;

	free_txt(&cyi->store);
	return 1;
}

DEF_CB(copy_do)
{
	struct copy_info *cyi = container_of(ci->comm, struct copy_info, cmd);

	if (strcmp(ci->key, "copy:save") == 0 && ci->str && ci->num == 0) {
		struct txt *t;

		if (cyi->store && strcmp(ci->str, cyi->store->txt) == 0)
			/* Identical to last save, don't bother */
			return 1;

		if (cyi->count >= 10) {
			struct txt **tp = &cyi->store;
			int cnt = 0;
			while (*tp && cnt < 10) {
				tp = &((*tp)->next);
				cnt += 1;
			}
			if (*tp)
				LOG("copy:save free %.20s", (*tp)->txt);
			free_txt(tp);
		}
		LOG("copy:save add %.20s", ci->str);
		t = calloc(1, sizeof(*t));
		t->next = cyi->store;
		t->txt = strdup(ci->str);
		cyi->store = t;
		return 1;
	}
	if (strcmp(ci->key, "copy:save") == 0 && ci->str && ci->num == 1) {
		/* Append str to the latest copy */
		struct txt *t;
		char *txt;

		LOG("copy:save append %.20s", ci->str);
		t = cyi->store;
		if (t) {
			txt = t->txt;
			t->txt = malloc(strlen(txt) + strlen(ci->str) + 1);
			strcat(strcpy(t->txt, txt), ci->str);
			free(txt);
		} else {
			t = calloc(1, sizeof(*t));
			t->next = cyi->store;
			t->txt = strdup(ci->str);
			cyi->store = t;
		}
		return 1;
	}
	if (strcmp(ci->key, "copy:get") == 0) {
		struct txt *t = cyi->store;
		int idx = ci->num;
		while (t && idx > 0) {
			t = t->next;
			idx -= 1;
		}
		if (t)
			LOG("copy:get %d returns %.20s", ci->num, t->txt);
		if (t)
			comm_call(ci->comm2, "callback", ci->focus, 0, NULL, t->txt);
		return 1;
	}
	return Efallthrough;
}

void edlib_init(struct pane *ed safe)
{
	struct copy_info *cyi;
	struct pane *p;

	if (!copy_map) {
		copy_map = key_alloc();
		key_add(copy_map, "Close", &copy_close);
	}

	p = pane_register(ed, 0, &copy_handle.c);
	if (!p)
		return;
	cyi = p->data;
	cyi->cmd = copy_do;
	cyi->pane = p;
	call_comm("global-set-command", ed, &cyi->cmd, 0, NULL, "copy:save");
	call_comm("global-set-command", ed, &cyi->cmd, 0, NULL, "copy:get");
}
