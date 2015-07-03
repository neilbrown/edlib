
struct pane *view_attach(struct pane *par, struct text *t, int border);
void view_register(struct map *m);
struct view_data {
	struct text	*text;
	struct point	*point;
};

