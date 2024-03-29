/*
 * Copyright Neil Brown ©2019-2023 <neil@brown.name>
 * May be distributed under terms of GPLv2 - see file:COPYING
 *
 * Declaration shared among core code, but not exported to
 * modules.
 */
#include "safe.h"

struct mark *doc_new_mark(struct pane *p safe, int view, struct pane *owner);
struct mark *safe point_dup(struct mark *p safe);
void points_resize(struct doc *d safe);
void points_attach(struct doc *d safe, int view);
struct mark *do_vmark_first(struct doc *d safe, int view, struct pane *owner safe);
struct mark *do_vmark_last(struct doc *d safe, int view, struct pane *owner safe);
struct mark *do_vmark_at_or_before(struct doc *d safe, struct mark *m safe, int view, struct pane *owner);
struct mark *do_mark_at_point(struct mark *pt safe, int view);
void do_mark_free(struct mark *m);

struct pane *pane_register_root(struct command *handle safe,
				void *data, short data_size);

void editor_delayed_free(struct pane *ed safe, struct pane *p safe);
void editor_delayed_mark_free(struct mark *m safe);
void doc_setup(struct pane *ed safe);
void log_setup(struct pane *ed safe);
void window_setup(struct pane *ed safe);
