int count_calculate(struct doc *d, struct mark *start, struct mark *end,
		    int *linep, int *wordp, int *charp);


struct pane *popup_register(struct pane *p, char *name, char *content, wint_t key);
void popup_init(void);

void render_text_attach(struct pane *p, struct point *pt);
void render_text_register(struct map *m);

void render_hex_register(struct map *m);
void render_hex_attach(struct pane *p);

