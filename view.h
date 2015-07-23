
struct pane *view_attach(struct pane *par, struct doc *t, int border);
void view_register(struct map *m);
struct view_data {
	struct doc	*doc;
	struct point	*point;
};

