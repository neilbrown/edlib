struct pane;
struct cmd_info;

struct command {
	int	(*func)(struct command *comm, int key, struct cmd_info *ci);
	char	*name;
};

struct map *key_alloc(void);
int key_lookup(struct map *m, int key, struct cmd_info *p);
void key_add(struct map *map, int k, struct command *comm);
void key_add_range(struct map *map, int first, int last,
		   struct command *comm);
struct command *key_register_mod(char *name, int *bit);

/* mouse numbers are:
 *  0-4 for button 0
 *  5-9 for button 1
 *  10-14 for button 2
 *  33 for movement
 * Each button can be
 *  0 - press
 *  1 - release
 *  2 - click
 *  3 - double-click
 *  4 - triple-click
 */
#define	M_PRESS(b)	((b)*5+0)
#define	M_RELEASE(b)	((b)*5+1)
#define	M_CLICK(b)	((b)*5+2)
#define	M_DCLICK(b)	((b)*5+3)
#define	M_TCLICK(b)	((b)*5+4)
#define	M_MOVE		33
#define	FK(k)		((k) | 0x1FFF00)
