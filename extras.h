void count_calculate(struct doc *d, struct mark *start, struct mark *end);

struct pane *popup_register(struct pane *p, char *name, char *content, char *key);
void popup_init(void);

struct map *emacs_register(void);
