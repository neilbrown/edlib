#include <limits.h>


/*
 * 0 - 10FFFF are Unicode keystrokes (or keyboard input at least)
 * edlib extra events are assigned from 1FFFFF downwards.
 * 1FFFxx are function keys using ncurses definitions
 * 1FFExx are mouse press/release/click and movement
 * 1FFDxx are ad-hoc functions that don't subdivide,
 *        like "search" or "replace"
 * 1FFCxx are movement commands, allowing different abstract units
 *
 * 200000 is a 'meta' flag.
 * 400000 is a 'super' flag.
 * It is assumed that shift/ctrl are encoded in the character,
 *
 * 0xFF8000000 allows for 512 mode modifiers.
 * These can be long-term modes (emacs or vi), temporary modes (vi insert)
 * or transient (emacs C-x etc).  There is a small stack for modifiers
 * and the top is always used - it is removed after use if it is transient.
 *
 * Mode 0 is reserved for modless commands which don't come from a key/mouse.
 * e.g. movement keys translate to an EV_MODE() command which is then sent to
 * the target pane.
 */

#define Kmod(k)		((k) & 0xFF800000)
#define	Kkey(k)		((k) & 0x001FFFFF)
#define	Kmeta(k)	((k) & 0x00200000)
#define	Ksuper(k)	((k) & 0x00400000)
#define	K_MOD(m,k)	((((m) & 0x1ff)<<23) | (k))

#define META(X)		((X) | (1<<21))
#define SUPER(X)	((X) | (1<<22))
#define	FUNC_KEY(k)	((k) | 0x1FFF00)

#define KCTRL(X) ((X) & 0x1f)

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
#define	MV_VIEW_ABSOLUTE EV_MOVE(34) /* numeric is a percentage, or other fraction */

#define	MV_CURSOR_XY	EV_MOVE(64)

#define	EV_USER_DEF(x)	(0x1FFB00 | ((x) & 0xff))
