/*
 * Copyright Neil Brown ©2019 <neil@brown.name>
 * May be distributed under terms of GPLv2 - see file:COPYING
 *
 * Declaration shared among core code, but not exported to
 * modules.
 */

struct mark *doc_new_mark(struct doc *d safe, int view, struct pane *owner);
struct mark *safe point_dup(struct mark *p safe);
wint_t mark_step2(struct doc *d safe, struct mark *m safe, int forward, int move);
void points_resize(struct doc *d safe);
void points_attach(struct doc *d safe, int view);
struct mark *do_vmark_first(struct doc *d safe, int view, struct pane *owner safe);
struct mark *do_vmark_last(struct doc *d safe, int view, struct pane *owner safe);
struct mark *do_vmark_at_point(struct doc *d safe, struct mark *pt safe, int view, struct pane *owner safe);
struct mark *do_vmark_at_or_before(struct doc *d safe, struct mark *m safe, int view, struct pane *owner);
void __mark_free(struct mark *m);
