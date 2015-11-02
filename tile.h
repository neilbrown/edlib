
struct pane *tile_init(struct pane *display);
struct pane *tile_split(struct pane *p, int horiz, int after);
void tile_register(struct map *m);
int tile_grow(struct pane *p, int horiz, int size);
