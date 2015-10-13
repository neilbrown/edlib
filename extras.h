void count_calculate(struct doc *d, struct mark *start, struct mark *end);

struct pane *popup_register(struct pane *p, char *name, char *content, wint_t key);
void popup_init(void);

void render_text_attach(struct pane *p, struct point *pt);
void render_hex_attach(struct pane *p);
void render_dir_attach(struct pane *p, struct point *pt);

void emacs_register(struct map *m);
