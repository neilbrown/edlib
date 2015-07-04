#include <limits.h>
struct pane;
struct cmd_info;

struct command {
	int	(*func)(struct command *comm, struct cmd_info *ci);
	char	*name;
	refresh_fn type;  /* pane of this type is provided as 'target' */
};

struct map *key_alloc(void);
int key_handle(struct cmd_info *ci);
int key_handle_focus(struct cmd_info *ci);
int key_handle_xy(struct cmd_info *ci);
void key_add(struct map *map, wint_t k, struct command *comm);
void key_add_range(struct map *map, wint_t first, wint_t last,
		   struct command *comm);
struct command *key_register_mod(char *name, int *bit);


/*
 * 0 - 10FFFF are Unicode keystrokes (or keyboard input at least)
 * edlib extra events are assigned from 1FFFFFF downwards.
 * 1FFFxx are function keys using ncurses definitions
 * 1FFExx are mouse press/release/click and movement
 * 1FFDxx are ad-hoc functions that don't subdivide,
 *        like "search" or "replace"
 * 1FFCxx are movement commands, allowing different abstract units
 */

#define	FUNC_KEY(k)	((k) | 0x1FFF00)

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
#define	M_KEY(ev)	((ev) | 0x1FFE00)
#define	M_PRESS(b)	M_KEY((b)*5+0)
#define	M_RELEASE(b)	M_KEY((b)*5+1)
#define	M_CLICK(b)	M_KEY((b)*5+2)
#define	M_DCLICK(b)	M_KEY((b)*5+3)
#define	M_TCLICK(b)	M_KEY((b)*5+4)
#define	M_MOVE		M_KEY(33)

#define	EV_SEARCH	(0x1FFD00)
#define	EV_REPLACE	(0x1FFD01)


#define	EV_MOVE(m)	((m)|0x1FFC00)
#define	MV_CHAR		EV_MOVE(0)
#define	MV_WORD		EV_MOVE(1)
#define	MV_WORD2	EV_MOVE(2)
#define	MV_EOL		EV_MOVE(3)
#define	MV_LINE		EV_MOVE(4) /* Move line, but stay in column */
#define	MV_SENTENCE	EV_MOVE(5)
#define	MV_PARAGRAPH	EV_MOVE(6)
#define	MV_SECTION	EV_MOVE(7)
#define	MV_CHAPTER	EV_MOVE(8)
#define	MV_UNIT		EV_MOVE(9) /* structural unit at current level */
#define	MV_LEVEL	EV_MOVE(10) /* Move to different level of units */
#define	MV_FILE		EV_MOVE(11) /* Start or End of file */

#define	MV_VIEW_SMALL	EV_MOVE(32) /* move view in lines, cursor stationary */
#define	MV_VIEW_LARGE	EV_MOVE(33) /* move view in pages */
#define	MV_VIEW_ABSOLUTE EV_MOVE(34) /* repeat is a percentage, or other fraction */

#define	MV_CURSOR_XY	EV_MOVE(64)

#define	EV_USER_DEF(x)	(0x1FFB00 | ((x) & 0xff))

/* Each event (above) is accompanied by a cmd_info structure.
 * 'key' and 'focus' are always present, others only if relevant.
 * Repeat is present for 'key' and 'move'.  INT_MAX means no number was
 *   requested so is usually treated like '1'.  Negative numbers are quite
 *   possible.
 * x,y are present for mouse events
 * 'str' is inserted by 'replace' and sought by 'search'
 * 'mark' is moved by 'move' and 'replace' deletes between point and mark.
 */
struct cmd_info {
	wint_t		key;
	struct pane	*focus;
	int		repeat;
	int		x,y;
	char		*str;
	struct mark	*mark;
	struct text	*text;
};

