/*
 * Copyright Neil Brown ©2015-2021 <neil@brown.name>
 * May be distributed under terms of GPLv2 - see file:COPYING
 *
 * rexel - A Regular EXpression Evaluation Library
 *    because everyone needs their own regex library
 *
 * This library supports matching without backtracking by providing
 * a single character at a time.  When a match is found, the
 * length of that match is reported.
 *
 * Compiled form of a regex is an array of 16 bit unsigned numbers called
 * rexels, or Regular EXpression ELements.
 * This involves some cheating as wctype_t (unsigned long int) values
 * are stored in 16 bits.
 * This array is comprised of a "regexp" section follow by some a "set"
 * section.
 * The first entry in the regex section is the size of that section (including
 * the length).  Adding this size to the start gives the start of the
 * "set" section.
 *
 * The "set" section contains some "sets" each of which contains 1 or
 * more subsections followed by a "zero".  Each subsection starts with
 * its size.  The first section can have size zero, others cannot (as
 * another zero marks the end of the "set").
 * The first subsection of a set is a list of "character classes".
 * An internal mapping is created from used character classes (like
 * "digit" and "lower" etc) to small numbers.  If a set should match a
 * given character class, the small number is stored in this subsection
 * If a set should *not* match, then the small number is added with the
 * msb set.
 *
 * Subsequent subsections contain a general character-set, each
 * subsection for a single unicode plane.  The top six bits of the first
 * entry is the plane number, the remaining bits are the size.  After
 * this are "size" 16bit chars in sorted order.  The values in even
 * slots are in the set, values in odd slots are not.  Value not in any
 * slot are treated like the largest value less than it which does have
 * a slot.  So iff a search for "largest entry nor larger than" finds an
 * even slot, then the target is in the set.
 *
 * The rexels in the "regexp" section come in 4 groups.
 *   0x: 15 bit unicode number.  Other unicode numbers cannot be matched
 *           this way, and must be matched with a "set".
 *   100: address of a "regex" subarray.   The match forks at this point,
 *        both the next entry and the addressed entry are considered.
 *        This limits total size to 8192 entries.
 *
 *   101: address of a char set.  This 13 bit address is an offset from the
 *        start of the "set" section.
 *
 *   110: start/end of a capture group.  High bit is 0 for start, 1 for end.
 *
 *   1110: must match the most recent instance of the capture group.
 *
 *   1111: reserve for various special purposes.
 *
 *   The last 16 value have special meanings.
 *     0xfff0 - match any char
 *     0xfff1 - match any character except and EOL character
 *     0xfff2 - match no char - dead end.
 *     0xfff3 - report success.
 *     0xfff4 - match at start of line
 *     0xfff5 - match at start of word
 *     0xfff6 - match at end of line
 *     0xfff7 - match at end of word
 *     0xfff8 - match a word break (start or end)
 *     0xfff9 - match any point that isn't a word break.
 *     0xfffa - match 1 or more spaces/tabs/newlines - lax searching.
 *     0xfffb - match - or _ - lax searching
 *     0xfffc - Subsequent chars (0x..) are matched ignoring case
 *     0xfffd - Subsequent chars are match case-correct.
 *
 * When matching, two pairs of extra arrays are allocated and used.
 * One pair is 'before', one pair is 'after'.  They swap on each char.
 * One contains a threaded linkage among all points in regex subarray
 * which are currently matched.  A 'zero' marks the end of the chain.
 * The other records the length of the longest match at that point.
 * So when a char is matched, the length+1 of the 'before' moves
 * to the 'after' position.
 *
 * A match is *before* processing the index command.
 *
 * "man 7 regex" described POSIX regular expression and notes some area
 * where implementations differ, using (!).  The terminology describes
 * a Regular Expression (RE) as:
 *  RE -> branch ( '|' branch ) *                # 1 or more branches
 *                                               # separated by '|'
 *  branch -> piece ( piece ) *                  # 1 or more pieces,
 *                                               # concatenated.
 *  piece ->  atom ( '*' | '+' | '?' | bound )?  # an atom, possibly
 *                                               # followed by repeater
 *  bound -> '{' N ( ',' ( N )? )? '}'           # 1 or 2 numbers in braces.
 *  atom ->  '(' RE ')' | C | '.' | \??          # dot or char or
 *                                               # RE in parentheses.
 *
 * Responding to each implementation difference:
 * - There must be at least one branch in an RE, and all must be non-empty.
 * - A branch needs at least one piece.
 * - This implementation (currently) only allows a *single* '*','+','?'
 *   after an atom.
 * - Integers in a bound must be less than 256.
 * - The empty string atom '()' is not permitted
 * - '\C', where 'C' is a special character: one of ^.[$()|*+?{\  removes any
 *    special meaning from that character.  This does not apply inside []
 *    as those characters have no special meaning, or a different meaning there.
 * - '\C', where 'C' is not in that list is an error except for those used for
 *    some special character classes.  Those classes which are not
 *    "everything except" are permitted equally inside character sets ([]).
 *    The classes are:
 *    \d a digit
 *    \p a punctuation character
 *    \s a spacing character
 *    \w a word (alphabetic character)
 *    \D \P \S \W  are negation of above.  So non-digit, non-punctuation etc.
 *    \A an upper case character
 *    \a a lower case character
 *
 * - A '{' followed by a non-digit is just a '{'
 * - Two ranges may *not* share an endpoint.
 * - equivalence classes and collating elements are not implemented.
 * - No particular limit on the length of an RE is imposed (yet)
 *
 *
 * Other extensions:
 * ?:    - a group that will never be captured
 * ?|    - don't capture, and within each branch any caputuring uses
 *         same numbering
 * ?0    - all remainder is literal
 * ?nnn:- a group of precisely nnn literal match chars
 * ?isLn-isLn:  - flags before any optional '-' are set. Flags after
 *         are cleared.
 *         i - ignore case
 *         L - lax matching for space and hyphen
 *         s - single line, '.' matches newline
 *         n - no capture in subgroups either
 *
 */

#include <stdlib.h>
#include <unistd.h>
#include <wchar.h>
#include <ctype.h>
#include <wctype.h>
#include <memory.h>
#ifdef DEBUG
#include <getopt.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#endif

#include "safe.h"
#include "misc.h"
#include "rexel.h"

#ifdef DEBUG
#include <stdio.h>
#endif
struct match_state {
	unsigned short	*rxl safe;
	unsigned short	* safe link[2];
	unsigned short	* safe leng[2];
	unsigned long	* safe ignorecase;
	unsigned short	active;
	bool		anchored;
	int		match;
	int		len;
	int		start, total;

	/* Backtracking state */
	bool		backtrack;
	/* 'buf' is an allocated buffer of 'buf_size'.
	 * There is text up to buf_count which we have been asked
	 * to match against.  We are currently considering the
	 * wchar at buf_pos to see if it matches.  If buf_pos == buf_count,
	 * rxl_advance_bt() will return so that another char can be provided.
	 */
	wint_t		* safe buf;
	unsigned short	buf_size;
	unsigned short	buf_pos, buf_count;
	/* pos in an index in 'rxl' that buf[buf_pos] must be matched against. */
	unsigned short	pos;
	/* 'record' is an allocated buffer of record_size of
	 * which record_count entries record details of the current match
	 * of buf..buf_pos from above.
	 * We only record fork points that were taken on the path through the
	 * pattern which achieved the current match.  During matching, we may
	 * need to rewind to these, and follow forward from that fork.
	 * During capture extraction, we need to follow those to find
	 * how the match applied.
	 */
	struct record {
		unsigned short pos;
		unsigned short len;
	}		* safe record;
	unsigned short	record_count;
	unsigned short	record_size;

	#ifdef DEBUG
	bool		trace;
	#endif
};

/* ignorecase is a bit set */
#define BITS_PER_LONG (sizeof(unsigned long) * 8)
static inline int BITSET_SIZE(int bits)
{
	return (bits + BITS_PER_LONG - 1) / BITS_PER_LONG;
}
static inline bool test_bit(int bit, const unsigned long *set safe)
{
	return !!(set[bit / BITS_PER_LONG] & (1 << (bit % BITS_PER_LONG)));
}

static inline void set_bit(int bit, unsigned long *set safe)
{
	set[bit / BITS_PER_LONG] |= (1 << (bit % BITS_PER_LONG));
}

static inline void clear_bit(int bit, unsigned long *set safe)
{
	set[bit / BITS_PER_LONG] &= ~(1 << (bit % BITS_PER_LONG));
}

#define	NO_LINK		0xFFFF
#define	LOOP_CHECK	0xFFFE

/* RExel Commands */
#define	REC_ANY		0xFFE0
#define	REC_ANY_NONL	0xFFE1
#define	REC_NONE	0xFFE2
#define	REC_MATCH	0xFFE3

#define	REC_SOL		0xFFE4
#define	REC_EOL		0xFFE5
#define	REC_SOW		0xFFE6
#define	REC_EOW		0xFFE7
#define	REC_WBRK	0xFFE8
#define	REC_NOWBRK	0xFFE9
#define	REC_SOD		0xFFEa	/* Start of Document */
#define	REC_EOD		0xFFEb	/* End of Document */
#define	REC_POINT	0xFFEc

#define	REC_LAXSPC	0xFFFc
#define	REC_LAXDASH	0xFFFd
#define	REC_IGNCASE	0xFFFe
#define	REC_USECASE	0xFFFf

#define	REC_FORK	0x8000
#define	REC_FORKLAST	0x8000
#define	REC_FORKFIRST	0x9000
#define	REC_SET		0xa000
#define	REC_CAPTURE	0xc000
#define	REC_CAPTURED	0xc800
/* 0xd??? unused */
#define	REC_BACKREF	0xe000
#define	REC_ISCHAR(x)	(!((x) & 0x8000))
#define	REC_ISSPEC(x)	((x) >= REC_ANY)
#define	REC_ISFORK(x)	(((x) & 0xe000) == REC_FORK)
#define	REC_ISSET(x)	(((x) & 0xe000) == REC_SET)
#define	REC_ISCAPTURE(x) (((x) & 0xf000) == REC_CAPTURE)
#define	REC_ISBACKREF(x) (((x) & 0xf000) == REC_BACKREF)
#define	REC_ADDR(x)	((x) & 0x0fff)
#define	REC_ISFIRST(x)	(!!((x) & (REC_FORKFIRST ^ REC_FORKLAST)))
#define	REC_CAPNUM(x)	((x) & 0x07ff)
#define	REC_CAPEND(x)	((x) & 0x0800)

static inline bool rec_noop(unsigned short cmd)
{
	/* These commands don't affect matching directly, they
	 * are extracted and used outside the core matcher.
	 */
	return cmd == REC_IGNCASE || cmd == REC_USECASE ||
		REC_ISCAPTURE(cmd);
}

static inline bool rec_zerowidth(unsigned short cmd)
{
	return ((cmd >= REC_SOL && cmd <= REC_NOWBRK) ||
		rec_noop(cmd));
}

/* First entry contains start of maps, and flags */
#define	RXL_PATNLEN(rxl)	((rxl)[0] & 0x3fff)
#define	RXL_SETSTART(rxl)	((rxl) + RXL_PATNLEN(rxl))

static int classcnt = 0;
static wctype_t *classmap safe = NULL;

static enum rxl_found rxl_advance_bt(struct match_state *st safe, wint_t ch);
static void bt_link(struct match_state *st safe, int pos, int len);
static int get_capture(struct match_state *s safe, int n, int which,
		       char *buf, int *startp);

/*
 * The match_state contains several partial matches that lead to "here".
 * rxl_advance() examines each of these to determine if they will still match
 * after consuming either a character or a position-type flag (SOL, EOL, etc).
 * It calls do_link for each case that is still a possible match.
 * 'pos' is the position in the regexp that matches the new point in the target.
 * 'dest' is the place in the new threaded list to record this match. i.e.
 *         the slot that it currently the end of the list.
 * 'len' is the length of the match of to this (new) point in the target.
 * If there is already a match to this point in the pattern, we just update
 * the length and don't relink anything.
 *
 */
static void do_link(struct match_state *st safe, int pos, int *destp, int len)
{
	unsigned short cmd = st->rxl[pos];

	while (rec_noop(cmd)) {
		pos += 1;
		cmd = st->rxl[pos];
	}
	if (cmd == REC_MATCH) {
		if (st->match < len)
			st->match = len;
		/* Don't accept another start point */
		st->anchored = True;
	}

	if (st->backtrack) {
		bt_link(st, pos, len);
		return;
	}

	if (!destp)
		return;
	if (cmd == REC_NOWBRK) {
		/* NOWBRK is special because it matches a character
		 * without consuming it.  We link them at the start
		 * of the list so they can be found quickly.
		 */
		if (st->link[st->active][pos] == NO_LINK) {
			st->leng[st->active][pos] = len;

			if (st->link[st->active][0] == 0)
				*destp = pos;
			st->link[st->active][pos] =
				st->link[st->active][0];
			st->link[st->active][0] = pos;
		} else if (st->leng[st->active][pos] < len)
			st->leng[st->active][pos] = len;
	} else if (!REC_ISFORK(cmd)) {
		/* not a FORK, so just link it in. */
		if (st->link[st->active][pos] == NO_LINK) {
			st->leng[st->active][pos] = len;

			st->link[st->active][*destp] = pos;
			st->link[st->active][pos] = 0;
			*destp = pos;
		} else if (st->leng[st->active][pos] < len)
			st->leng[st->active][pos] = len;
	} else if (st->link[st->active][pos] == NO_LINK ||
		   st->leng[st->active][pos] < len) {
		st->link[st->active][pos] = LOOP_CHECK;
		st->leng[st->active][pos] = len;
		do_link(st, REC_ADDR(cmd), destp, len);
		do_link(st, pos+1, destp, len);
	}
}

static int set_match(struct match_state *st safe, unsigned short addr,
		     wchar_t ch, bool ic)
{
	unsigned short *set safe = RXL_SETSTART(st->rxl) + addr;
	wchar_t uch = ch, lch = ch;
	unsigned short len;
	bool invert = False;

	if (ic) {
		/* As Unicode has 3 cases, can we be sure that everything
		 * has a 'lower' to map to?  Surely everything has at least
		 * and upper or a lower...
		 */
		uch = towupper(ch);
		lch = towlower(ch);
	}
	/* First there is an 'invert' flag and possibly some char classes */
	len = *set++;
	invert = !!(len & 0x8000);
	if (len) {
		len &= 0x7fff;
		for ( ; len--; set++)
			if (iswctype(uch, classmap[*set]) ||
			    (uch != lch && iswctype(lch, classmap[*set])))
				return !invert;
	}
	/* now there might be some sets.  Each set starts with a size with
	 * top 5 bytes indicating top bytes of unicode planes, and bottom
	 * 11 bytes size of table
	 */
	while ((len = *set++) != 0) {
		int high = (len & 0xF800) << 5;
		/* Both upper and lower case have been placed in set,
		 * so only need to search for one of them
		 */
		unsigned short target;
		int lo, hi;

		len &= 0x7ff;
		if ((uch & 0x1f0000) == high)
			target = uch & 0xffff;
		else if ((lch & 0x1f0000) == high)
			target = lch & 0xffff;
		else {
			set += len;
			continue;
		}
		/* Binary search to find first entry that is greater
		 * than target.
		 */
		lo = 0; /* Invar: No entry before lo is greater than target */
		hi = len; /* Invar: Every entry at or after hi is greater
			   * than target */
#ifdef DEBUG
		/* Sanity check - array must be sorted */
		for (lo = 1; lo < len; lo++)
			if (set[lo-1] >= set[lo]) {
				printf("Set %d, subset %d not ordered at %u\n",
				       addr, set - RXL_SETSTART(st->rxl) - addr,
				       lo);
				exit(1);
			}
		lo = 0;
#endif

		while (lo < hi) {
			int mid = (lo + hi) / 2;
			if (set[mid] > target)
				hi = mid;
			else
				lo = mid + 1;
		}
		/* set[lo] == set[hi] = first entry greater than target.
		 * If 'lo' is even, there was no match.  If 'lo' is odd,
		 * there was.
		 */
		if (lo & 1)
			return !invert;
		set += len;
	}
	return invert;
}

/*
 * Advance the match state to process 'ch' or a flag.
 * flag indicates start/end of word/line.
 * Returns -2 if there is no possibility of a match including this ch/flag
 * Returns -1 if part of the pattern has matched, and more input is needed.
 * Returns >=0 if a match has been found.  The return value is the number
 *  of characters (not flags) in the match.
 * When a >=0 return is given, it might still be useful to keep calling
 * rxl_advance if a maximal match is wanted.
 * If the match must be anchor to the first character, this must have been
 * advised to rxl_prepare.  The caller should keep calling until no chars are
 * left, or -2 is returned (no match), or >=0 is returned.  In the latter case
 * the call might keep calling to see if a longer match is possible.  Until -2
 * is seen, a longer match is still possible.
 */

static int advance_one(struct match_state *st safe, int i,
		       wint_t ch, int flag,
		       int len, int *eolp)
{
	unsigned int cmd = st->rxl[i];
	wint_t lch = ch, uch = ch;
	bool ic = test_bit(i, st->ignorecase);
	int advance = 0;

	if (ic) {
		uch = toupper(ch);
		lch = tolower(ch);
	}

	if (REC_ISSPEC(cmd)) {
		switch(cmd) {
		case REC_ANY:
			advance = 1;
			if (flag)
				advance = 0;
			break;
		case REC_ANY_NONL:
			advance = 1;
			if (ch == '\n' || ch == '\r' || ch == '\f')
				advance = -1;
			if (flag)
				advance = 0;
			break;
		case REC_MATCH:
			/* cannot match more chars here */
			if (flag)
				advance = 0;
			else
				advance = -1;
			break;
		case REC_NONE:
			advance = -1;
			break;
		case REC_SOL:
			if (flag & RXL_SOL)
				advance = 1;
			else if (!flag)
				advance = -1;
			else
				advance = 0;
			break;
		case REC_EOL:
			if (flag & RXL_EOL)
				advance = 1;
			else if (!flag)
				advance = -1;
			else
				advance = 0;
			break;
		case REC_SOD:
			if (flag & RXL_SOD)
				advance = 1;
			else if (!flag)
				advance = -1;
			else
				advance = 0;
			break;
		case REC_EOD:
			if (flag & RXL_EOD)
				advance = 1;
			else if (!flag)
				advance = -1;
			else
				advance = 0;
			break;
		case REC_POINT:
			if (flag & RXL_POINT)
				advance = 1;
			else if (!flag)
				advance = -1;
			else
				advance = 0;
			break;
		case REC_SOW:
			if (flag & RXL_SOW)
				advance = 1;
			else if (flag & RXL_NOWBRK || !flag)
				advance = -1;
			else
				advance = 0;
			break;
		case REC_EOW:
			if (flag & RXL_EOW)
				advance = 1;
			else if (flag & RXL_NOWBRK || !flag)
				advance = -1;
			else
				advance = 0;
			break;
		case REC_WBRK:
			if (flag & (RXL_SOW | RXL_EOW))
				advance = 1;
			else if (flag & RXL_NOWBRK || !flag)
				advance = -1;
			else
				advance = 0;
			break;
		case REC_NOWBRK:
			if (flag & (RXL_SOW | RXL_EOW))
				advance = -1;
			else if (flag & RXL_NOWBRK)
				advance = 1;
			else
				advance = 0;
			break;
		case REC_LAXSPC:
			if (strchr(" \t\r\n\f", ch) != NULL)
				advance = 1;
			else
				advance = -1;
			if (flag)
				advance = 0;
			break;
		case REC_LAXDASH:
			if (strchr("-_.", ch) != NULL)
				advance = 1;
			else
				advance = -1;
			if (flag)
				advance = 0;
			break;
		}
	} else if (flag) {
		/* expecting a char, so ignore position info */
		advance = 0;
	} else if (REC_ISCHAR(cmd)) {
		if (cmd == ch ||
		    (ic && (cmd == uch || cmd == lch)))
			advance = 1;
		else
			advance = -1;
	} else if (REC_ISSET(cmd)) {
		if (set_match(st, REC_ADDR(cmd), ch, ic))
			advance = 1;
		else
			advance = -1;
	} else if (REC_ISBACKREF(cmd)) {
		/* Backref not supported */
		advance = -2;
	} else
		/* Nothing else is possible here */
		abort();
	if (advance < 0)
		/* no match on this path */
		;
	else if (advance == 0)
		/* Nothing conclusive here */
		do_link(st, i, eolp, len);
	else
		/* Need to advance and link the new address in.  However
		 * if there is a fork, we might need to link multiple
		 * addresses in.  Best use recursion.
		 */
		do_link(st, i+1, eolp, len + (ch != 0));
	return advance;
}

enum rxl_found rxl_advance(struct match_state *st safe, wint_t ch)
{
	int active;
	int next;
	int eol;
	unsigned short i;
	wint_t flag = ch & ~(0x1fffff);
	enum rxl_found ret = RXL_NOMATCH;

	if (st->backtrack)
		return rxl_advance_bt(st, ch);

	ch &= 0x1fffff;

	if (flag && ((flag & (flag-1)) || ch)) {
		int f;
		enum rxl_found r;
		/* Need to handle flags separately */
		for (f = RXL_SOD ; f <= RXL_EOD; f <<= 1) {
			if (!(flag & f))
				continue;
			r = rxl_advance(st, f);
			if (r > ret)
				ret = r;
		}
		flag = 0;
		if (!ch)
			return ret;
	}
	active = st->active;
	next = 1-active;

	if (flag == RXL_NOWBRK &&
	    (st->link[active][0] == NO_LINK ||
	     st->rxl[st->link[active][0]] != REC_NOWBRK))
		/* Reporting not-a-word-boundary, but no-one cares,
		 * so just return.
		 */
		return st->match >= 0 ? RXL_MATCH_FLAG :
			st->len >= 0 ? RXL_CONTINUE : RXL_NOMATCH;

	if (!flag)
		st->total += 1;

	if (!st->anchored) {
		/* We haven't found a match yet and search is not anchored,
		 * so possibly start a search here.
		 */
		eol = 0;
		while (st->link[active][eol])
			eol = st->link[active][eol];
		/* Found the end of the list. */
		do_link(st, 1, &eol, 0);
	}
	st->match = -1;
	eol = 0;
	st->active = next;
#ifdef DEBUG
	if (st->trace) {
		/* Trace shows current length at each state. FORK points
		 * are not state points
		 * At each point we print the Char or Set number, then the
		 * length of a match to there - on the next line.
		 * Allow 4 chars per column
		 */
		char t[5];
		unsigned short cnt;
		int len = RXL_PATNLEN(st->rxl);

		for (i = 1; i < len; i++)
			if (!REC_ISFORK(st->rxl[i])) {
				unsigned short cmd = st->rxl[i];
				if (REC_ISCHAR(cmd)) {
					if (cmd > ' ' && cmd < 0x7f)
						printf("'%c' ", cmd);
					else
						printf("x%3x", cmd);
				} else if (REC_ISSET(cmd)) {
					printf("S%-3d", REC_ADDR(cmd));
				} else if (REC_ISBACKREF(cmd)) {
					printf("B%-3d", REC_CAPNUM(cmd));
				} else if (REC_ISCAPTURE(cmd)) {
					if (REC_CAPEND(cmd))
						printf("%3d)", REC_CAPNUM(cmd));
					else
						printf("(%-3d", REC_CAPNUM(cmd));
				} else switch(cmd) {
					case REC_ANY: printf(" .  "); break;
					case REC_ANY_NONL: printf(" .? "); break;
					case REC_NONE:printf("##  "); break;
					case REC_SOL: printf(" ^  "); break;
					case REC_EOL: printf(" $  "); break;
					case REC_SOW: printf("\\<  "); break;
					case REC_EOW: printf("\\>  "); break;
					case REC_SOD: printf("\\`  "); break;
					case REC_EOD: printf("\\'  "); break;
					case REC_POINT: printf("\\=  "); break;
					case REC_WBRK: printf("\\b  "); break;
					case REC_NOWBRK: printf("\\B "); break;
					case REC_MATCH:printf("!!! "); break;
					case REC_LAXSPC: printf("x20!"); break;
					case REC_LAXDASH: printf("-!  "); break;
					case REC_IGNCASE: printf("?i:"); break;
					case REC_USECASE: printf("?c:"); break;
					default: printf("!%04x", cmd);
					}
			}
		printf("\n");
		for (i = 1 ; i < len; i++)
			if (!REC_ISFORK(st->rxl[i])) {
				if (st->link[active][i] == NO_LINK)
					printf("--  ");
				else
					printf("%2d  ", st->leng[active][i]);
			}
		if (flag) {
			printf("Flag:");
			if (flag & RXL_SOD) printf(" SOD");
			if (flag & RXL_SOL) printf(" SOL");
			if (flag & RXL_EOL) printf(" EOL");
			if (flag & RXL_EOD) printf(" EOD");
			if (flag & RXL_SOW) printf(" SOW");
			if (flag & RXL_EOW) printf(" EOW");
			if (flag & RXL_NOWBRK) printf(" NOWBRK");
			if (flag & RXL_POINT) printf(" POINT");
		} else
			printf("Match %s(%x)",
			       ch < ' ' ? "?" : put_utf8(t, ch) , ch);

		/* Now check the linkage is correct.  The chain should lead
		 * to 0 without seeing any 'NO_LINK' or any ISFORK, and
		 * the number of NO_LINK plus number on chain should make len
		 */
		cnt = 0;
		i = 0;
		do {
			if (st->link[active][i] == NO_LINK)
				abort();
			if (i && REC_ISFORK(st->rxl[i]))
				abort();
			cnt += 1;
			i = st->link[active][i];
		} while (i);
		for (i = 0; i < len; i++)
			if (st->link[active][i] == NO_LINK ||
			    st->link[active][i] == LOOP_CHECK)
				cnt += 1;
		if (cnt != len)
			abort();
	}
#endif /* DEBUG */
	/* Firstly, clear out next lists */
	/* This works before NO_LINK is 0xffff */
	memset(st->link[next], 0xff, RXL_PATNLEN(st->rxl) * 2);
	memset(st->leng[next], 0, RXL_PATNLEN(st->rxl) * 2);
	st->link[next][0] = 0;

	/* Now advance each current match */
	for (i = st->link[active][0]; i; i = st->link[active][i]) {
		int len = st->leng[active][i];

		advance_one(st, i, ch, flag, len, &eol);
	}
	st->link[next][eol] = 0;
	if (st->match > st->len) {
		st->len = st->match;
		st->start = st->total - st->len;
	}
	#ifdef DEBUG
	if (st->trace) {
		if (st->match >= 0 || eol != 0)
			printf(" ... -> %d\n", st->match);
		else
			printf(" ... -> NOMATCH\n");
	}
	#endif
	if (ret >= RXL_MATCH_FLAG || (st->match >= 0 && flag))
		return RXL_MATCH_FLAG;
	if (ret >= RXL_MATCH || st->match >= 0)
		return RXL_MATCH;
	if (eol == 0 && st->match < 0 && st->anchored) {
		/* No chance of finding (another) match now */
		return RXL_DONE;
	}
	if (st->len >= 0)
		return RXL_CONTINUE;
	return RXL_NOMATCH;
}

int rxl_info(struct match_state *st safe, int *lenp safe, int *totalp,
	      int *startp, int *since_startp)
{
	*lenp = st->len;
	if (totalp)
		*totalp = st->total;
	if (startp) {
		if (st->len < 0)
			*startp = -1;
		else
			*startp = st->start;
	}
	if (since_startp) {
		if (st->len < 0)
			*since_startp = -1;
		else
			*since_startp = st->total - st->start;
	}
	/* Return 'true' if there might be something here and
	 * we cannot safely skip forward
	 */
	return st->anchored ||
		(st->link[st->active] && st->link[st->active][0] != 0);
}

#define RESIZE(buf)							\
	do {								\
		if ((buf##_size) <= (buf##_count+1)) {			\
			if ((buf##_size) < 8)				\
				(buf##_size) = 8;			\
			(buf##_size) *= 2;				\
			(buf) = realloc(buf, (buf##_size) * sizeof((buf)[0]));\
		}							\
	} while(0);

static void bt_link(struct match_state *st safe, int pos, int len)
{
	unsigned short cmd = st->rxl[pos];

	if (!REC_ISFORK(cmd)) {
		st->pos = pos;
		return;
	}

	RESIZE(st->record);
	st->record[st->record_count].pos = pos;
	st->record[st->record_count].len = len;
	st->record_count += 1;
	if (REC_ISFIRST(cmd))
		/* priority fork - follow the fork */
		do_link(st, REC_ADDR(cmd), NULL, len);
	else
		/* just continue for now, fork later */
		do_link(st, pos + 1, NULL, len);
}

static enum rxl_found rxl_advance_bt(struct match_state *st safe, wint_t ch)
{
	/* This is a back-tracking version of rxl_advance().  If the new
	 * ch/flag matches, we store it and return.  If it doesn't, we
	 * backtrack and retry another path until we run out of all
	 * paths, or find a path where more input is needed.
	 */

	if (st->anchored && st->record_count == 0 && st->pos == 0)
		return RXL_DONE;

	if (st->len >= 0)
		return RXL_DONE;

	RESIZE(st->buf);
	st->buf[st->buf_count++] = ch;
	st->total += 1;

	st->match = -1;
	do {
		wint_t flags;
		int f = 1;;
		ch = st->buf[st->buf_pos++];
		flags = ch & ~0x1FFFFF;
		ch &= 0x1FFFFF;

		for (f = RXL_SOD ; f && f <= RXL_EOD*2; f <<= 1) {
			int i = st->pos;
			int r;

			#ifdef DEBUG
			if (!st->trace)
				;
			else if (f == RXL_EOD*2) {
				char t[5];
				printf("%d: Match %s(%x)", i,
				       ch < ' ' ? "?" : put_utf8(t, ch) , ch);
			} else if (f & flags){
				printf("%d: Flag:", i);
				if (f & RXL_SOD) printf(" SOD");
				if (f & RXL_EOD) printf(" EOD");
				if (f & RXL_SOL) printf(" SOL");
				if (f & RXL_EOL) printf(" EOL");
				if (f & RXL_SOW) printf(" SOW");
				if (f & RXL_EOW) printf(" EOW");
				if (f & RXL_NOWBRK) printf(" NOWBRK");
				if (f & RXL_POINT) printf(" POINT");
			}
			#endif

			if (f == RXL_EOD*2 && ch)
				r = advance_one(st, i, ch, 0,
						st->buf_pos - 1, NULL);
			else if (f == RXL_EOD*2)
				r = -1;
			else if (f & flags)
				r = advance_one(st, i, 0, f,
						st->buf_pos - 1, NULL);
			else
				continue;

			if (r == -2) {
				/* REC_BACKREF cannot be handled generically
				 * by advance_one.  We need to find the
				 * referenced string and match - or ask for more.
				 */
				int prevpos;
				int start;
				int len = get_capture(st, REC_CAPNUM(st->rxl[i]), 0,
						      NULL, &start);
				st->buf_pos -= 1;
				prevpos = st->buf_pos;
				while (len > 0 && st->buf_pos < st->buf_count) {
					ch = st->buf[st->buf_pos++] & 0x1FFFFF;
					if ((st->buf[start] & 0x1FFFFF) == ch) {
						start += 1;
						len -= 1;
					} else {
						/* Failure to match */
						len = -1;
					}
				}
				if (len > 0) {
					/* Need more input */
					st->buf_pos = prevpos;
					return RXL_CONTINUE;
				}
				if (len == 0) {
					/* Successful match */
					do_link(st, i+1, NULL, st->buf_pos);
					r = 1;
				}
			}

			if (r >= 0) {
				/* st->pos has been advanced if needed */
				if (st->match > st->len) {
					st->len = st->match;
					st->start = st->total - st->buf_count;
				}
				#ifdef DEBUG
				if (st->trace)
					printf(" -- OK(%d %d %d/%d)\n", r,
					       st->buf_pos,
					       st->start, st->len);
				#endif
				if (st->len >= 0)
					return RXL_MATCH;
				continue;
			}

			/* match failed - backtrack */
			if (st->record_count > 0) {
				st->record_count -= 1;
				st->pos = st->record[st->record_count].pos;
				st->buf_pos = st->record[st->record_count].len;
				#ifdef DEBUG
				if (st->trace)
					printf(" -- NAK backtrack to %d/%d\n",
					       st->pos, st->buf_pos);
				#endif
				if (REC_ISFIRST(st->rxl[st->pos]))
					/* priority jump, just step forward now */
					do_link(st, st->pos+1, NULL, st->buf_pos);
				else
					/* delayed jump, take it now */
					do_link(st, REC_ADDR(st->rxl[st->pos]),
						NULL, st->buf_pos);
			} else {
				/* cannot backtrack, so skip first char unless anchored */
				#ifdef DEBUG
				if (st->trace)
					printf(" -- NAK - %s\n",
					       st->anchored?"abort":"fail");
				#endif
				if (st->anchored) {
					st->pos = 0;
					return RXL_DONE;
				}
				st->buf_count -= 1;
				memmove(st->buf, st->buf+1,
					sizeof(st->buf[0]) * st->buf_count);
				st->buf_pos = 0;
				do_link(st, 1, NULL, st->buf_pos);
			}
			break;
		}
	} while (st->buf_pos < st->buf_count);

	if (st->len < 0)
		return RXL_NOMATCH;
	else
		return RXL_CONTINUE;
}

enum modifier {
	IgnoreCase	= 1,
	LaxMatch	= 2,
	SingleLine	= 4,
	DoCapture	= 8,
	CapturePerBranch=16,
};

struct parse_state {
	const char	*patn safe;
	unsigned short	*rxl;
	int		next;
	unsigned short	*sets;
	int		set;	/* Next offset to store a set */
	enum modifier mod;	/* Multiple 'or'ed together */
	int		capture;

	/* Details of set currently being parsed */
	int		len;
};

static void add_cmd(struct parse_state *st safe, unsigned short cmd)
{
	if (st->rxl)
		st->rxl[st->next] = cmd;
	st->next += 1;
}

static void relocate(struct parse_state *st safe, unsigned short start, int len)
{
	int i;
	if (!st->rxl) {
		st->next += len;
		return;
	}
	for (i = st->next-1; i >= start; i-=1) {
		unsigned short cmd = st->rxl[i];
		if (REC_ISFORK(cmd) &&
		    REC_ADDR(cmd) >= start)
			cmd += len;
		st->rxl[i+len] = cmd;
	}
	st->next += len;
}

static wint_t cvt_hex(const char *s safe, int len)
{
	long rv = 0;
	while (len) {
		if (!*s || !isxdigit(*s))
			return WERR;
		rv *= 16;
		if (*s <= '9')
			rv += *s - '0';
		else if (*s <= 'F')
			rv += *s - 'A' + 10;
		else if (*s <= 'f')
			rv += *s - 'a' + 10;
		else
			abort();
		s++;
		len--;
	}
	return rv;
}

static wint_t cvt_oct(const char **sp safe, int maxlen)
{
	const char *s = *sp;
	long rv = 0;
	while (maxlen) {
		if (!s || !*s || !isdigit(*s) || *s == '8' || *s == '9')
			break;
		rv *= 8;
		rv += *s - '0';
		s++;
		maxlen--;
	}
	*sp = s;
	return rv;
}

static bool __add_range(struct parse_state *st safe, wchar_t start, wchar_t end,
			int plane, int *planes safe, int *newplane safe)
{
	int p;
	int lo, hi;
	int len;
	unsigned short *set;
	if (end < start)
		return False;
	if (!st->sets) {
		/* guess 2 entries for each plane, plus 1
		 * if we add a plane.  Each plane needs an extra slot
		 * if the set is inverted.
		 */
		for (p = (start & 0x1F0000)>>16;
		     p <= (end & 0x1F0000)>>16 ;
		     p++) {
			if (!((*planes) & (1 << p))) {
				*planes |= 1 << p;
				st->len += 1;
			}
			st->len += 2;
		}
		/* All planes handled, so set *newplane beyond
		 * the last.
		 */
		*newplane = 0x11 << 16;
		return True;
	}
	/* OK, for real this time, need to build up set 'plane' */
	if (start >= ((plane+1) << 16)) {
		/* Nothing to do for this plane, move to 'start' */
		*newplane = start >> 16;
		return True;
	}
	if (end < (plane << 16)) {
		/* nothing more to do */
		*newplane = 0x11 << 16;
		return True;
	}
	/* Contract range to this plane */
	if (start < (plane << 16))
		start = plane << 16;
	if (end >= ((plane+1) << 16))
		end = ((plane+1) << 16) - 1;
	if (!((*planes) & (1 << plane))) {
		st->sets[st->set] = (plane << 11);
		*planes |= 1 << plane;
	}
	/* now clip to 16bit */
	start &= 0xFFFF;
	end &= 0xFFFF;

	/* Now insert range into the list.
	 * 1/ Perform search for 'start'.
	 * 2/ If at 'even' offset then not present yet.
	 *   2a/ if 'start-1' is present, update that to end
	 *   2b/ if next is <= end, update that to start
	 *   2c/ otherwise shift up and insert range - done.
	 * 3/ if at 'odd' offset then is in already
	 *   3a/ if next is beyond 'end', then done
	 *   3b/ otherwise update next to end
	 * 4/ while ranges over-lap, delete two endpoints
	 *    and shift down.
	 */

	len = st->len;
	set = st->sets + st->set + 1;
	/* Binary search to find first entry that is greater
	 * than target.
	 */
	lo = 0; /* Invar: No entry before lo is greater than target */
	hi = len; /* Invar: Every entry at or after hi is greater than target */
	while (lo < hi) {
		int mid = (lo + hi) / 2;
		if (set[mid] > start)
			hi = mid;
		else
			lo = mid + 1;
	}
	/* set[lo] == set[hi] = first entry greater than target.
	 * If 'lo' is even, there was no match.  If 'lo' is odd,
	 * there was.
	 */
	if ((lo & 1) == 0) {
		/* Not yet present.*/
		if (lo > 0 && set[lo-1] == start) {
			/* Extend the earlier range */
			lo -= 1;
			if (end == 0xffff)
				len = lo;
			else
				set[lo] = end+1;
		} else if (lo < len && set[lo] <= end+1)
			set[lo] = start;
		else {
			/* need to insert */
			memmove(set+lo+2, set+lo, sizeof(set[lo])*(len-lo));
			set[lo] = start;
			if (end == 0xffff)
				len = lo+1;
			else {
				set[lo+1] = end+1;
				len += 2;
			}
		}
	} else {
		/* Already present, lo is end of a range, or beyond len */
		if (lo == len || set[lo] > end)
			/* nothing to do */;
		else
			set[lo] = end+1;
	}
	lo |= 1;
	/* Lo now points to the end of a range. If it overlaps the next,
	 * merge the ranges.
	 */
	while (lo+1 < len && set[lo] >= set[lo+1]) {
		/* Need to merge these ranges */
		if (lo+2 < len){
			if (set[lo] > set[lo+2])
				set[lo+2] = set[lo];
			memmove(set+lo, set+lo+2,
				sizeof(set[lo])*(len - (lo+2)));
		}
		len -= 2;
	}
	st->len = len;
	return True;
}

static bool add_range(struct parse_state *st safe, wchar_t start, wchar_t end,
		      int plane, int *planes safe, int *newplane safe)
{
	if (!(st->mod & IgnoreCase) ||
	    !iswalpha(start) || !iswalpha(end))
		return __add_range(st, start, end, plane, planes, newplane);
	if (!__add_range(st, towlower(start), towlower(end),
			plane, planes, newplane))
		return False;
	return __add_range(st, towupper(start), towupper(end),
			   plane, planes, newplane);
}

static void add_class(struct parse_state *st safe, int plane, wctype_t cls)
{
	int c;
	if (!st->sets) {
		/* one entry required per class */
		st->len += 1;
		return;
	} else if (plane >= 0)
		/* already handled. */
		return;

	for (c = 0; c < classcnt ; c++)
		if (classmap[c] == cls)
			break;
	if (c < classcnt) {
		st->sets[st->set + (++st->len)] = c;
		return;
	}
	if ((classcnt & (classcnt - 1)) == 0) {
		/* need to allocate space */
		int size;
		if (classcnt)
			size = classcnt * 2;
		else
			size = 8;
		classmap = realloc(classmap, size * sizeof(classmap[0]));
	}

	classmap[classcnt++] = cls;
	st->sets[st->set + (++st->len)] = c;
	return;
}

static bool is_set_element(const char *p safe)
{
	int i;

	if (p[0] != '.' && p[0] != '=' && p[0] != ':')
		return False;
	for (i = 1; p[i]; i++)
		if (p[i] == ']') {
			if (p[i-1] == p[1] && i > 1)
				return True;
			else
				return False;
		}
	return False;
}

static int do_parse_set(struct parse_state *st safe, int plane)
{
	const char *p safe = st->patn;
	wint_t ch;
	int newplane = 0xFFFFFF;
	int planes = 0;
	int invert;
	/* first characters are special... */
	invert = 0;
	st->len = 0;
	if (*p == '^') {
		invert = 1;
		p += 1;
	}
	do {
		ch = get_utf8(&p, NULL);
		if (ch == '\\' && p[0] && strchr("0xuUnrft", p[0]) != NULL) {
			switch (*p++) {
			case '0': ch = cvt_oct(&p, 3);  break;
			case 'x': ch = cvt_hex(p, 2); p += 2; break;
			case 'u': ch = cvt_hex(p, 4); p += 4; break;
			case 'U': ch = cvt_hex(p, 8); p += 8; break;
			case 't': ch = '\t'; break;
			case 'n': ch = '\n'; break;
			case 'r': ch = '\r'; break;
			case 'f': ch = '\f'; break;
			}
			if (ch == WEOF)
				return -1;
		}

		if (ch >= WERR) {
			return -1;
		} else if (ch == '[' && is_set_element(p)) {
			switch(p[0]) {
			case '.': /* collating set */
			case '=': /* collating element */
				/* FIXME */
				st->patn = p;
				return -1;
			case ':': /* character class */
			{
				const char *e;
				char *cls;
				wctype_t wct;
				p += 1;
				e = strchr(p, ':');
				if (!e)
					e = p + strlen(p);
				cls = strndup(p, e-p);
				wct = wctype(cls);
				free(cls);
				if (!wct)
					return -1;
				p = e;
				while (*p && *p != ']')
					p++;
				p++;
				add_class(st, plane, wct);
				break;
			}
			}
		} else if (p[0] == '-' && p[1] != ']') {
			/* range */
			wint_t ch2;
			p += 1;
			ch2 = get_utf8(&p, NULL);
			if (ch2 >= WERR ||
			    !add_range(st, ch, ch2, plane, &planes, &newplane))
				return -1;
		} else if (p[0] == ']' && (p == st->patn ||
					   (invert && p == st->patn+1))) {
			if (!add_range(st, ch, ch, plane, &planes, &newplane))
				return -1;
		} else if (ch == '\\' && p[0] > 0 && p[0] < 0x7f && p[1] != '-'
			   && strchr("daApsw", p[0]) != NULL) {
			switch (p[0]) {
			case 'd': add_class(st, plane, wctype("digit")); break;
			case 'a': add_class(st, plane, wctype("lower")); break;
			case 'A': add_class(st, plane, wctype("upper")); break;
			case 'p': add_class(st, plane, wctype("punct")); break;
			case 's': add_class(st, plane, wctype("space")); break;
			case 'h': add_class(st, plane, wctype("blank")); break;
			case 'w': add_class(st, plane, wctype("alpha")); break;
			}
			p += 1;
		} else if (ch) {
			if (!add_range(st, ch, ch, plane, &planes, &newplane))
				return -1;
		}
	} while (*p != ']');
	st->patn = p+1;
	if (st->sets) {
		if (plane < 0) {
			/* We have a (possibly empty) class list. Record size */
			unsigned short l = st->len;
			if (invert)
				l |= 0x8000;
			st->sets[st->set] = l;
		} else {
			/* We have a set, not empty.  Store size */
			st->sets[st->set] = st->len;
		}
	}
	st->set += st->len+1;
	return newplane;
}

static bool parse_set(struct parse_state *st safe)
{
	int plane;
	const char *patn;
	int set;

	if (*st->patn++ != '[')
		return False;
	/* parse the set description multiple times if necessary
	 * building up each sub table one at a time.
	 * First time through we do classes, and report which set
	 * to do next.  Then again for each Unicode plane that
	 * is needed
	 * do_parse_set returns -1 on error, next plane number needed,
	 * or a number larger than any valid plane number when done.
	 * When pre-parsing to calculate sizes, we guess the sizes on a single
	 * walk through - possibly over-estimating.
	 */
	set = st->set;
	plane = -1; /* Code for "parse classes" */
	patn = st->patn;
	do {
		st->patn = patn;
		plane = do_parse_set(st, plane);
	} while (plane >= 0 && plane <= 0x100000);
	if (plane < 0)
		return False;
	if (st->sets)
		st->sets[st->set] = 0;
	st->set++;
	add_cmd(st, REC_SET | set);
	return True;
}

static unsigned short add_class_set(struct parse_state *st safe,
				    char *cls safe, int in)
{
	if (!st->rxl /* FIXME redundant, rxl and sets are set at same time */
	    || !st->sets) {
		st->set += 3;
		return REC_SET;
	}
	st->sets[st->set] = in ? 1 : 0x8001;
	st->len = 0;
	add_class(st, -1, wctype(cls));
	st->sets[st->set + 2] = 0;
	st->set += 3;
	return REC_SET | (st->set - 3);
}
static bool parse_re(struct parse_state *st safe, int capture);
static bool parse_atom(struct parse_state *st safe)
{
	/* parse out an atom: one of:
	 * (re)
	 * [set]
	 * .
	 * \special
	 * ^
	 * $
	 * char - including UTF8
	 *
	 * If there is a syntax error, return False, else return True;
	 *
	 */
	wint_t ch;

	if (*st->patn == '\0')
		return False;
	if (*st->patn == '.') {
		if (st->mod & SingleLine)
			add_cmd(st, REC_ANY);
		else
			add_cmd(st, REC_ANY_NONL);
		st->patn++;
		return True;
	}
	if (*st->patn == '(') {
		st->patn++;
		if (!parse_re(st, 0))
			return False;
		if (*st->patn != ')')
			return False;
		st->patn++;
		return True;
	}
	if (*st->patn == '^') {
		add_cmd(st, REC_SOL);
		st->patn++;
		return True;
	}
	if (*st->patn == '$') {
		if (isdigit(st->patn[1])) {
			/* $n and $nn is a backref */
			ch = st->patn[1] - '0';
			st->patn += 2;
			if (isdigit(st->patn[0])) {
				ch = ch * 10 + st->patn[0] - '0';
				st->patn += 1;
			}
			add_cmd(st, REC_BACKREF | ch);
			return True;
		}
		add_cmd(st, REC_EOL);
		st->patn++;
		return True;
	}
	if (*st->patn == '[')
		return parse_set(st);
	if ((st->mod & LaxMatch) &&
	    st->patn[0] == ' ' && st->patn[1] != ' ' && st->patn[1] != '\t' &&
	    (st->next == 1 || (st->patn[-1] != ' ' && st->patn[-1] != '\t'))) {
		add_cmd(st, REC_LAXSPC);
		/* LAXSPC can be repeated */
		add_cmd(st, REC_FORKFIRST | (st->next - 1));
		st->patn++;
		return True;
	}
	if ((st->mod & LaxMatch) &&
	    (st->patn[0] == '-' || st->patn[0] == '_')) {
		add_cmd(st, REC_LAXDASH);
		st->patn++;
		return True;
	}
	if (*st->patn & 0x80) {
		ch = get_utf8(&st->patn, NULL);
		if (ch >= WERR)
			return False;
		st->patn -= 1;
	} else
		ch = *st->patn;
	if (ch == '\\') {
		st->patn++;
		ch = *st->patn;
		switch (ch) {
			/* These just fall through and are interpreted
			 * literally */
		case '^':
		case '.':
		case '[':
		case ']':
		case '$':
		case '(':
		case ')':
		case '|':
		case '*':
		case '+':
		case '?':
		case '{':
		case '}':
		case '\\':
			break;
			/* These are simple translations */
		case '`': ch = REC_SOD; break;
		case '\'':ch = REC_EOD; break;
		case '=': ch = REC_POINT; break;
		case '<': ch = REC_SOW; break;
		case '>': ch = REC_EOW; break;
		case 'b': ch = REC_WBRK; break;
		case 'B': ch = REC_NOWBRK; break;
		case 't': ch = '\t'; break;
		case 'n': ch = '\n'; break;
		case 'r': ch = '\r'; break;
		case 'f': ch = '\f'; break;
		case '0': ch = cvt_oct(&st->patn, 4);
			st->patn -= 1;
			break;
		case 'x': ch = cvt_hex(st->patn+1, 2);
			if (ch >= WERR)
				return False;
			st->patn += 2;
			break;
		case 'u': ch = cvt_hex(st->patn+1, 4);
			if (ch >= WERR)
				return False;
			st->patn += 4;
			break;
		case 'U': ch = cvt_hex(st->patn+1, 8);
			if (ch >= WERR)
				return False;
			st->patn += 8;
			break;
		case '1'...'9': ch = st->patn[0] - '0';
			while (st->patn[1] >= '0' && st->patn[1] <= '9') {
				ch = ch * 10 + st->patn[1] - '0';
				st->patn += 1;
			}
			ch |= REC_BACKREF;
			break;
		case 'd': ch = add_class_set(st, "digit", 1); break;
		case 'D': ch = add_class_set(st, "digit", 0); break;
		case 's': ch = add_class_set(st, "space", 1); break;
		case 'S': ch = add_class_set(st, "space", 0); break;
		case 'h': ch = add_class_set(st, "blank", 1); break;
		case 'H': ch = add_class_set(st, "blank", 0); break;
		case 'w': ch = add_class_set(st, "alpha", 1); break;
		case 'W': ch = add_class_set(st, "alpha", 0); break;
		case 'p': ch = add_class_set(st, "punct", 1); break;
		case 'P': ch = add_class_set(st, "punct", 0); break;

		case 'a': ch = add_class_set(st, "lower", 1); break;
		case 'A': ch = add_class_set(st, "upper", 0); break;

			/* Anything else is an error (e.g. \0) or
			 * reserved for future use.
			 */
		default: return False;
		}
	}
	add_cmd(st, ch);
	st->patn++;
	return True;
}

static bool parse_piece(struct parse_state *st safe)
{
	int start = st->next;
	char c;
	int min, max;
	char *ep;
	int skip = 0;
	int nongreedy = 0;

	if (!parse_atom(st))
		return False;
	c = *st->patn;
	if (c != '*' && c != '+' && c != '?' &&
	    !(c=='{' && isdigit(st->patn[1])))
		return True;

	st->patn += 1;
	switch(c) {
	case '*':
		/* make spare for 'jump forward' */
		relocate(st, start, 1);
		if (st->patn[0] == '?') {
			st->patn += 1;
			/* non-greedy match */
			add_cmd(st, REC_FORKLAST | (start+1));
			if (st->rxl)
				st->rxl[start] = REC_FORKFIRST | st->next;
		} else {
			add_cmd(st, REC_FORKFIRST | (start+1));
			if (st->rxl)
				st->rxl[start] = REC_FORKLAST | st->next;
		}
		return True;
	case '+':
		/* just (optional) jump back */
		if (st->patn[0] == '?') {
			st->patn += 1;
			/* non-greedy */
			add_cmd(st, REC_FORKLAST | start);
		} else
			add_cmd(st, REC_FORKFIRST | start);
		return True;
	case '?':
		/* Just a jump-forward */
		relocate(st, start, 1);
		if (st->patn[0] == '?') {
			st->patn += 1;
			if (st->rxl)
				st->rxl[start] = REC_FORKFIRST | st->next;
		} else {
			if (st->rxl)
				st->rxl[start] = REC_FORKLAST | st->next;
		}
		return True;
	case '{':/* Need a number, maybe a comma, if not maybe a number,
		  * then }, and optionally a '?' after that.
		  */
		min = strtoul(st->patn, &ep, 10);
		if (min > 256 || !ep)
			return False;
		max = min;
		if (*ep == ',') {
			max = -1;
			ep++;
			if (isdigit(*ep)) {
				max = strtoul(ep, &ep, 10);
				if (max > 256 || max < min || !ep)
					return False;
			}
		}
		if (*ep != '}')
			return False;
		if (ep[1] == '?') {
			nongreedy = (REC_FORKLAST ^ REC_FORKFIRST);
			ep += 1;
		}
		st->patn = ep+1;
		/* Atom need to be repeated min times, and maybe as many
		 * as 'max', or indefinitely if max < 0
		 */
		while (min > 1) {
			/* Make a duplicate */
			int newstart = st->next;
			relocate(st, start, st->next - start);
			start = newstart;
			min -= 1;
			max -= 1;
		}
		if (min == 0) {
			/* Need to allow the atom to be skipped */
			relocate(st, start, 1);
			if (st->rxl) {
				st->rxl[start] = (REC_FORKLAST^nongreedy) | st->next;
				skip = start;
			}
			start += 1;
		}
		if (max < 0) {
			add_cmd(st, (REC_FORKFIRST^nongreedy) | start);
		} else if (max > 1) {
			/* need to duplicate atom but make each one optional */
			int len = st->next - start;
			int last = st->next + (len + 1) * (max-1);
			if (skip && st->rxl) {
				st->rxl[skip] = (REC_FORKLAST^nongreedy) | last;
			}
			while (max > 1) {
				int newstart;
				add_cmd(st, (REC_FORKLAST^nongreedy) | last);
				newstart = st->next;
				relocate(st, start, len+1);
				st->next -= 1;
				start = newstart;
				max -= 1;
			}
			if (last != st->next)
				abort();
		}
		return True;
	}
	return False;
}

static bool parse_branch(struct parse_state *st safe)
{
	do {
		if (!parse_piece(st))
			return False;
		switch (*st->patn) {
		case '*':
		case '+':
		case '?':
			/* repeated modifier - illegal */
			return False;
		}
	} while (*st->patn && *st->patn != '|' && *st->patn != ')');
	return True;
}

/* rxl_fatch_match
 * like 'strstr', but length is passed for both 'needle' and
 * 'haystack', and it also finds a match for a prefix of needle
 * at the very end.
 * This can be used to accelerate search for verbatim content.
 */
int rxl_fast_match(const char *needle safe, int nlen,
		   const char *haystack safe, int hlen)
{
	int ret = 0;

	while (hlen >= nlen) {
		int nl = nlen;
		int i = 0;

		while (haystack[i] == needle[i]) {
			i++;
			nl--;
			if (!nl)
				return ret;
		}
		haystack++;
		hlen--;
		ret++;
	}
	/* Maybe a suffix of haystack is a prefix of needle */
	while (hlen) {
		int nl = hlen;
		int i = 0;

		while (haystack[i] == needle[i]) {
			i++;
			nl--;
			if (!nl)
				return ret;
		}
		haystack++;
		hlen--;
		ret++;
	}
	return ret;
}

static int parse_prefix(struct parse_state *st safe)
{
	/* A prefix to an re starts with '?' and 1 or more
	 * chars depending on what the next char is.
	 * A () group that starts '?' is never captured.
	 *
	 * ?:  no-op.  Only effect is to disable capture  This applies
	 *     recursively.
	 * ?|  groups in each branch use same capture numbering
	 * ?0  All chars to end of pattern are literal matches.  This cannot
	 *     be used inside ()
	 * ?N..: where N is a digit 1-9, it and all digits upto ':' give the
	 *       number of chars for literal pattern.  This must be the whole re,
	 *       so either ) follows, or end-of-pattern.
	 * ?iLsn-iLsn:
	 *     Each letter represents a flag which is set if it appears before
	 *     '-' and cleared if it appears after.  The '-' is optional, but
	 *     the ':' isn't.
	 *   i  ignore case in this group
	 *   L  lax matching for space and hyphen in this group
	 *   s  single-line.  '.' matches newline in this group
	 *   n  no capture in subgroups unless 'n' is explicitly re-enabled.
	 *      So if 'n' is already disabled, you need
	 *       (?n:(regexp))
	 *      to create a captured group
	 *   any other flag before the ':' causes an error
	 *
	 * Return:
	 * -1  bad prefix
	 * 0  no prefix
	 * 1  good prefix, st->mod updated if needed.
	 * 2  literal parsed, don't parse further for this re.
	 */
	const char *s;
	int verblen = 0;
	char *ve;
	bool neg = False;

	if (*st->patn != '?')
		return 0;
	s = st->patn + 1;
	switch (*s) {
	default:
		return -1;
	case ':':
		break;
	case '|':
		st->mod |= CapturePerBranch;
		break;
	case '0':
		verblen = -1;
		break;
	case '-':
	case 'i':
	case 'L':
	case 's':
	case 'n':
		for (; *s && *s != ':'; s++)
			switch (*s) {
			case '-':
				neg = True;
				break;
			case 'i':
				st->mod |= IgnoreCase;
				if (neg)
					st->mod &= ~IgnoreCase;
				break;
			case 'L':
				st->mod |= LaxMatch;
				if (neg)
					st->mod &= ~LaxMatch;
				break;
			case 's':
				st->mod |= SingleLine;
				if (neg)
					st->mod &= ~SingleLine;
				break;
			case 'n':
				st->mod &= ~DoCapture;
				if (neg)
					st->mod |= DoCapture;
				break;
			case ':':
				break;
			default:
				return -1;
			}
		if (*s != ':')
			return -1;
		break;
	case '1' ... '9':
		verblen = strtoul(s, &ve, 10);
		if (!ve || *ve != ':')
			return -1;
		s = ve;
		break;
	}
	s += 1;
	st->patn = s;
	if (verblen == 0)
		return 1;
	while (verblen && *s) {
		wint_t ch;
		if (*s & 0x80) {
			ch = get_utf8(&s, NULL);
			if (ch >= WERR)
				return 0;
		} else
			ch = *s++;
		add_cmd(st, ch);
		verblen -= 1;
	}
	st->patn = s;
	return 2;
}

static bool parse_re(struct parse_state *st safe, int capture_flag)
{
	int re_start = st->next;
	int start = re_start;
	int save_mod = st->mod;
	int ret;
	int capture = st->capture;
	int capture_start, capture_max;
	int do_capture = st->mod & DoCapture;

	switch (parse_prefix(st)) {
	case -1: /* error */
		return False;
	case 0:
		break;
	case 1:
		/* Don't capture if inside () */
		do_capture = capture_flag;
		break;
	case 2:
		/* Fully parsed - no more is permitted */
		return True;
	}
	if ((st->mod ^ save_mod) & IgnoreCase) {
		/* change of ignore-case status */
		if (st->mod & IgnoreCase)
			add_cmd(st, REC_IGNCASE);
		else
			add_cmd(st, REC_USECASE);
	}

	if (do_capture) {
		add_cmd(st, REC_CAPTURE | capture);
		st->capture += 1;
	}
	capture_start = capture_max = st->capture;
	while ((ret = parse_branch(st)) != 0 && *st->patn == '|') {
		st->patn += 1;
		relocate(st, start, 1);
		if (st->rxl)
			st->rxl[start] = REC_FORKLAST | (st->next + 2);
		add_cmd(st, REC_FORKLAST | start); /* will become 'jump to end' */
		add_cmd(st, REC_NONE);
		start = st->next;
		if (st->mod & CapturePerBranch) {
			/* Restart capture index each time */
			if (st->capture > capture_max)
				capture_max = st->capture;
			st->capture = capture_start;
		}
	}
	if (ret && st->rxl) {
		/* Need to patch all the "jump to end" links */
		while (start > re_start) {
			unsigned short cmd = st->rxl[start - 2];
			st->rxl[start - 2] = REC_FORKLAST | st->next;
			start = REC_ADDR(cmd);
		}
	}
	if (st->mod & CapturePerBranch && st->capture < capture_max)
		st->capture = capture_max;
	if (do_capture)
		add_cmd(st, REC_CAPTURED | capture);
	if ((st->mod ^ save_mod) & IgnoreCase) {
		/* restore from char of ignore-case status */
		if (save_mod & IgnoreCase)
			add_cmd(st, REC_IGNCASE);
		else
			add_cmd(st, REC_USECASE);
	}
	st->mod = save_mod;
	return ret;
}

unsigned short *rxl_parse(const char *patn safe, int *lenp, int nocase)
{
	struct parse_state st;
	st.patn = patn;
	st.mod = nocase ? IgnoreCase | LaxMatch : 0;
	st.mod |= DoCapture;
	st.rxl = NULL;
	st.next = 1;
	st.sets = NULL;
	st.set = 0;
	st.capture = 0;
	if (nocase)
		add_cmd(&st, REC_IGNCASE);
	if (!parse_re(&st, DoCapture) || *st.patn != '\0') {
		if (lenp)
			*lenp = st.patn - patn;
		return NULL;
	}
	add_cmd(&st, REC_MATCH);
	st.rxl = malloc((st.next + st.set) * sizeof(st.rxl[0]));
	st.rxl[0] = st.next;
	st.sets = st.rxl + st.next;
	st.patn = patn;
	st.mod = nocase ? IgnoreCase | LaxMatch : 0;
	st.mod |= DoCapture;
	st.next = 1;
	st.set = 0;
	st.capture = 0;
	if (nocase)
		add_cmd(&st, REC_IGNCASE);
	if (!parse_re(&st, DoCapture))
		abort();
	add_cmd(&st, REC_MATCH);
	return st.rxl;
}

unsigned short *safe rxl_parse_verbatim(const char *patn safe, int nocase)
{
	struct parse_state st;
	wint_t ch;

	st.next = 1 + !!nocase + strlen(patn) + 1;
	st.rxl = malloc(st.next * sizeof(st.rxl[0]));
	st.rxl[0] = st.next;
	st.next = 1;
	if (nocase)
		add_cmd(&st, REC_IGNCASE);
	while ((ch = get_utf8(&patn, NULL)) < WERR)
		add_cmd(&st, ch);
	add_cmd(&st, REC_MATCH);
	return st.rxl;
}

int rxl_prefix(unsigned short *rxl safe, char *ret safe, int max)
{
	/* Return in ret a contant prefix of the search string,
	 * if there is one.  Report the number of bytes.
	 */
	int len = RXL_PATNLEN(rxl);
	int i;
	int found = 0;

	for (i = 1; i < len; i++) {
		if (REC_ISCHAR(rxl[i])) {
			int l = utf8_bytes(rxl[i]);
			if (l == 0 || found + l > max)
				/* No more room */
				break;
			put_utf8(ret + found, rxl[i]);
			found += l;
			continue;
		}
		if (rxl[i] >= REC_SOL && rxl[i] <= REC_POINT)
			/* zero-width match doesn't change prefix */
			continue;
		if (rxl[i] == REC_USECASE)
			/* Insisting on case-correct doesn't change prefix */
			continue;
		if (REC_ISCAPTURE(rxl[i]))
			continue;
		/* Nothing more that is fixed */
		break;
	}
	return found;
}

struct match_state *safe rxl_prepare(unsigned short *rxl safe, int flags)
{
	struct match_state *st;
	int len = RXL_PATNLEN(rxl);
	int i;
	bool ic = False;
	int eol;

	st = malloc(sizeof(*st));
	memset(st, 0, sizeof(*st));
	st->rxl = rxl;
	st->anchored = flags & RXL_ANCHORED;
	st->backtrack = flags & RXL_BACKTRACK;
	if (!st->backtrack) {
		st->link[0] = malloc(len * sizeof(unsigned short) * 4);
		st->link[1] = st->link[0] + len;
		st->leng[0] = st->link[1] + len;
		st->leng[1] = st->leng[0] + len;
	}
	st->ignorecase = calloc(BITSET_SIZE(len), sizeof(*st->ignorecase));
	st->active = 0;
	st->match = -1;
	for (i = 1; i < len; i++) {
		if (rxl[i] == REC_IGNCASE)
			ic = True;
		if (rxl[i] == REC_USECASE)
			ic = False;
		if (ic)
			set_bit(i, st->ignorecase);
		if (!st->backtrack) {
			st->link[0][i] = i ? NO_LINK : 0;
			st->link[1][i] = i ? NO_LINK : 0;
		}
	}

	/* Set linkage to say we have a zero-length match
	 * at the start state.
	 */
	eol = 0;
	st->len = -1;
	do_link(st, 1, &eol, 0);

	return st;
}

void rxl_free_state(struct match_state *s)
{
	if (s) {
		free(s->link[0]);
		free(s->ignorecase);
		free(s->buf);
		free(s->record);
		free(s);
	}
}

static int get_capture(struct match_state *s safe, int n, int which,
		       char *buf, int *startp)
{
	int bufp = 0; /* Index in s->buf */
	int rxlp = 1; /* Index in s->rxl */
	int recp = 0; /* Index in s->rec */
	int rxll = RXL_PATNLEN(s->rxl);
	int start = 0;
	int len = -1;

	while (rxlp < rxll && recp <= s->record_count) {
		unsigned short cmd = s->rxl[rxlp];

		if (REC_ISFORK(cmd)) {
			bool recorded = False;
			if (recp < s->record_count &&
			    s->record[recp].pos == rxlp &&
			    s->record[recp].len == bufp) {
				recorded = True;
				recp += 1;
			}
			if (recorded
			    ==
			    REC_ISFIRST(cmd))
				/* This fork was taken */
				rxlp = REC_ADDR(cmd);
			else
				/* Fork not taken */
				rxlp += 1;
			continue;
		}
		if (REC_ISCAPTURE(cmd) &&
		    REC_CAPNUM(cmd) == n) {
			if (REC_CAPEND(cmd)) {
				len = bufp - start;
				if (--which == 0)
					break;
			} else {
				start = bufp;
				len = -1;
			}
		}
		if (rec_zerowidth(cmd)) {
			rxlp += 1;
			continue;
		}
		bufp += 1;
		rxlp += 1;
	}
	if (which > 0)
		return -1;
	if (startp)
		*startp = start;
	if (len > 0 && buf) {
		int i;
		for (i = 0; i < len; i++)
			buf[i] = s->buf[start+i] & 0xff;
		buf[i] = 0;
	}
	return len;
}

int rxl_capture(struct match_state *st safe, int cap, int which,
		int *startp safe, int *lenp safe)
{
	int len = get_capture(st, cap, which, NULL, startp);

	*lenp = len;
	return len >= 0;
}

static int interp(struct match_state *s safe, const char *form safe, char *buf)
{
	int len = 0;

	while (*form) {
		int which, n = 0;
		char *e;

		if (form[0] != '\\') {
			if (buf)
				buf[len] = *form;
			len += 1;
			form += 1;
			continue;
		}
		if (form[1] == '\\') {
			if (buf)
				buf[len] = '\\';
			len += 1;
			form += 2;
			continue;
		}
		/* must be \NN to get last of group NN or
		 * \:NN:MM to get MMth of grop NN
		 */
		if (isdigit(form[1])) {
			which = strtoul(form+1, &e, 10);
		} else if (form[1] == ':' && isdigit(form[2])) {
			which = strtoul(form+2, &e, 10);
			if (!e || e[0] != ':' || !isdigit(e[1]))
				return True;
			n = strtoul(e+2, &e, 10);
		} else
			return -1;

		n = get_capture(s, which, n, buf ? buf+len : buf, NULL);
		len += n;
		if (!e)
			break;
		form = e;
	}
	if (buf)
		buf[len] = 0;
	return len;
}

char *rxl_interp(struct match_state *s safe, const char *form safe)
{
	int size;
	char *ret;

	if (!s->backtrack)
		return NULL;
	size = interp(s, form, NULL);
	if (size < 0)
		return NULL;
	ret = malloc(size+1);
	interp(s, form, ret);
	return ret;
}

#ifdef DEBUG
#include <locale.h>
static void printc(unsigned short c)
{
	if (c <= ' ' || c >= 0x7f)
		printf("\\x%02x", c);
	else
		printf("%c", c);
}

static void print_set(unsigned short *set safe)
{
	int len = *set++;
	int invert = len & 0x8000;

	len &= 0x7fff;
	if (len)
		printf("[%s", invert?"^":"");
	while (len--) {
		unsigned short class = *set++;
		printf(":%d", class);
		if (!len)
			printf("]");
	}
	while ((len = *set++) != 0) {
		printf("%d:[", len);
		while (len > 0) {
			printc(*set);
			if (len > 1) {
				printf(",");
				set += 1;
				printc(set[0]);
				len -= 1;
			}
			set += 1;
			len -= 1;
			if (len)
				printf(";");
		}
		printf("]");
	}
}

void rxl_print(unsigned short *rxl safe)
{
	unsigned short *set = RXL_SETSTART(rxl);
	unsigned short *i;

	for (i = rxl+1 ; i < set; i++) {
		unsigned short cmd = *i;
		char t[5];
		printf("%04u: ", i-rxl);
		if (REC_ISCHAR(cmd))
			printf("match %s (#%x)\n", put_utf8(t, cmd), cmd);
		else if (REC_ISSPEC(cmd)) {
			switch(cmd) {
			case REC_ANY: printf("match ANY\n"); break;
			case REC_ANY_NONL: printf("match ANY-non-NL\n"); break;
			case REC_NONE: printf("DEAD END\n"); break;
			case REC_SOL: printf("match start-of-line\n"); break;
			case REC_EOL: printf("match end-of-line\n"); break;
			case REC_SOW: printf("match start-of-word\n"); break;
			case REC_EOW: printf("match end-of-word\n"); break;
			case REC_SOD: printf("match start-of-doc\n"); break;
			case REC_EOD: printf("match end-of-doc\n"); break;
			case REC_POINT: printf("match focus-point\n"); break;
			case REC_MATCH: printf("MATCHING COMPLETE\n"); break;
			case REC_WBRK: printf("match word-break\n"); break;
			case REC_NOWBRK: printf("match non-wordbreak\n"); break;
			case REC_LAXSPC: printf("match lax-space\n"); break;
			case REC_LAXDASH: printf("match lax-dash\n"); break;
			case REC_IGNCASE: printf("switch ignore-case\n"); break;
			case REC_USECASE: printf("switch use-case\n"); break;
			default: printf("ERROR %x\n", cmd); break;
			}
		} else if (REC_ISFORK(cmd))
			printf("branch to %d %s\n", REC_ADDR(cmd),
			       REC_ISFIRST(cmd) ? "first" : "later");
		else if (REC_ISSET(cmd)) {
			printf("Match from set %d: ", REC_ADDR(cmd));
			print_set(set + REC_ADDR(cmd));
			printf("\n");
		} else if (REC_ISBACKREF(cmd)) {
			printf("Backref to capture %d\n", REC_CAPNUM(cmd));
		} else if (REC_ISCAPTURE(cmd) && !REC_CAPEND(cmd)) {
			printf("Start Capture %d\n", REC_CAPNUM(cmd));
		} else if (REC_ISCAPTURE(cmd)) {
			printf("End   Capture %d\n", REC_CAPNUM(cmd));
		} else
			printf("ERROR %x\n", cmd);
	}
}

enum {
	F_VERB = 1,
	F_ICASE = 2,
	F_PERR = 4,
	F_BACKTRACK = 8,
};
static struct test {
	char *patn, *target;
	int flags, start, len;
	char *form, *replacement;
} tests[] = {
	{ "abc", "the abc", 0, 4, 3},
	{ "abc\\'", "the abc", 0, 4, 3},
	{ "\\`abc", "the abc", 0, -1, -1},
	{ "abc\\=", "abcabcPointabc", 0, 3, 3},
	{ "abc", "the ABC", F_ICASE, 4, 3},
	{ "a*", " aaaaac", 0, 0,  0},
	{ "a*", "aaaaac", 0, 0,  5},
	{ "a*", "AaAaac", F_ICASE, 0,  5},
	{ "a+", " aaaaac", 0, 1,  5},
	{ "?0()+*", " (()+***", 0, 2, 4},
	{ ".(?2:..).", "abcdefg..hijk", 0, 6, 4},
	{ "?L:1 2", "hello 1 \t\n 2", 0, 6, 6},
	{ "?s:a.c", "a\nc abc", 0, 0, 3},
	{ "?-s:a.c", "a\nc abc", 0, 4, 3},
	{ "ab[]^-]*cd", "xyab-^^]-cd", 0, 2, 9},
	//case insensitive
	{ "?i:(from|to|cc|subject|in-reply-to):", "From: bob", 0, 0, 5 },
	// Inverting set of multiple classes
	{ "[^\\A\\a]", "a", 0, -1, -1},
	// Search for start of a C function: non-label at start of line
	{ "^([^ a-zA-Z0-9#]|[\\A\\a\\d_]+[\\s]*[^: a-zA-Z0-9_])", "hello:  ",
	 0, -1, -1},
	// Match an empty string
	{ "[^a-z]*", "abc", 0, 0, 0},
	{ "^[^a-z]*", "abc", 0, 0, 0},
	// repeated long string - should match longest
	{ "(hello |there )+", "I said hello there hello to you", 0, 7, 18},
	// pattern not complete
	{ "extra close paren)", "anyting", F_PERR, -2, -2},
	// word boundaries
	{ "\\bab", "xyabc acab abc a", 0, 11, 2},
	{ "\\<ab", "xyabc acab abc a", 0, 11, 2},
	{ "ab\\>", "ababa abaca ab a", 0, 12, 2},
	{ "ab\\b", "ababa abaca ab", 0, 12, 2},
	{ "\\Bab", "ababa abaca ab", 0, 2, 2},
	{ "[0-9].\\Bab", "012+ab 45abc", 0, 7, 4},
	{ "[\\060-\\x09].\\Bab", "012+ab 45abc", 0, 7, 4},
	// octal chars
	{ "[\\0101\\0102\\x43\\u0064]*", "ABCddCBA1234", 0, 0, 8},
	{ "\\011", "a\tb", 0, 1, 1},
	// special controls
	{ "[\t\r\f\n]*\t\r\f\n", "trfn\t\r\f\n\t\r\f\n", 0, 4, 8},
	// backref for backtracking only
	{ "(.(.).)\\1", "123123", F_BACKTRACK, 0, 6},
	// backre must skip partial match
	{ "a(bcdef)$1", "abcdefbc abcdefbcdefg", F_BACKTRACK, 9, 11},
	// lax matching
	{ "hello there-all", "Hello\t  There_ALL-youse", F_ICASE, 0, 17},
	{ "hello there-all", "Hello\t  There_ALL-youse", 0, -1, -1},
	{ "^[^a-zA-Z0-9\n]*$", "=======", 0, 0, 7},
	{ "^$", "", 0, 0, 0},
	{ "a\\S+b", " a b axyb ", 0, 5, 4},
	{ "a[^\\s]+b", " a b axyb ", 0, 5, 4},
	{ "a[^\\s123]+b", " a b a12b axyb ", 0, 10, 4},
	{ "([\\w\\d]+)\\s*=\\s*(.*[^\\s])", " name = some value ", 0, 1, 17,
	 "\\1,\\2", "name,some value"},
	{ "?|foo(bar)|(bat)foo", "foobar", 0, 0, 6, "\\1", "bar"},
	{ "?|foo(bar)|(bat)foo", "batfoo", 0, 0, 6, "\\1", "bat"},
	// compare greedy and non-greedy
	{ "(.*)=(.*)", "assign=var=val", 0, 0, 14, "\\1..\\2", "assign=var..val"},
	{ "(.*?)=(.*)", "assign=var=val", 0, 0, 14, "\\1..\\2", "assign..var=val"},
	{ "(.+)=(.*)", "assign=var=val", 0, 0, 14, "\\1..\\2", "assign=var..val"},
	{ "(.+?)=(.*)", "assign=var=val", 0, 0, 14, "\\1..\\2", "assign..var=val"},
	{ "(.{5,15})=(.*)", "assign=var=val", 0, 0, 14, "\\1..\\2", "assign=var..val"},
	{ "(.{5,15}?)=(.*)", "assign=var=val", 0, 0, 14, "\\1..\\2", "assign..var=val"},
	{ "(.?)[a-e]*f", "abcdef", 0, 0, 6, "\\1", "a"},
	{ "(.?""?)[a-e]*f", "abcdef", 0, 0, 6, "\\1", ""},
	{ "diff|(stg|git).*show", "git diff", 0, 4, 4},
	// \h matches space but not newline
	{ "spa\\hce", "spa\nce spa ce", 0, 7, 6},
	// \s matches  newline
	{ "spa\\sce", "spa\nce spa ce", 0, 0, 6},
};

static void run_tests(bool trace)
{
	int cnt = sizeof(tests) / sizeof(tests[0]);
	int i;
	int alg;

	for (i = 0, alg = 0;
	     alg <= 1;
	     i < cnt-1 ? i++ : (i = 0, alg++)) {
		int flags;
		int f = tests[i].flags;
		char *patn = tests[i].patn;
		const char *target = tests[i].target;
		unsigned short *rxl;
		int mstart, mlen;
		int len, ccnt = 0;
		enum rxl_found r;
		wint_t prev;
		struct match_state *st;

		if (trace)
			printf("Match %s against %s using %s\n", patn, target,
			       alg ? "backtrack" : "parallel");
		if (f & F_VERB)
			rxl = rxl_parse_verbatim(patn, f & F_ICASE);
		else
			rxl = rxl_parse(patn, &len, f & F_ICASE);
		if (!rxl) {
			if (f & F_PERR) {
				if (trace)
					printf("Parse error as expected\n\n");
				continue;
			}
			printf("test %d(%s): Parse error at %d\n",
			       i, patn, len);
			exit(1);
		}
		if (f & F_PERR) {
			printf("test %d(%s): No parse error found\n", i, patn);
			exit (1);
		}

		if (trace)
			rxl_print(rxl);
		st = rxl_prepare(rxl, alg ? RXL_BACKTRACK : 0);
		st->trace = trace;

		flags = RXL_SOL|RXL_SOD;
		prev = L' ';
		do {
			wint_t wc = get_utf8(&target, NULL);
			if (wc >= WERR)
				break;
			if (iswalnum(prev) && !iswalnum(wc))
				flags |= RXL_EOW;
			else if (!iswalnum(prev) && iswalnum(wc))
				flags |= RXL_SOW;
			else
				flags |= RXL_NOWBRK;
			if (wc == 'P' && strncmp(target, "oint", 4) == 0)
				flags |= RXL_POINT;
			prev =  wc;
			r = rxl_advance(st, wc | flags);
			flags = 0;
			ccnt += 1;
		} while (r != RXL_DONE);
		if (*target == 0) {
			flags |= RXL_EOL|RXL_EOD;
			if (iswalnum(prev))
				flags |= RXL_EOW;
			rxl_advance(st, flags);
		}
		if (trace)
			printf("\n");
		rxl_info(st, &mlen, NULL, &mstart, NULL);
		if ((f & F_BACKTRACK) && !alg) {
			/* This is only expected to work for backtracking */
			if (mstart != -1 || mlen != -1) {
				printf("test %d(%s) %s: succeeded unexpectedly %d/%d\n",
				       i, patn, "parallel",
				       mstart, mlen);
			}
		} else if (tests[i].start != mstart ||
			   tests[i].len != mlen) {
			printf("test %d(%s) %s: found %d/%d instead of %d/%d\n",
			       i, patn,
			       alg ? "backtracking" : "parallel",
			       mstart, mlen, tests[i].start, tests[i].len);
			exit(1);
		}
		if (alg && tests[i].form && tests[i].replacement) {
			char *new;
			if (trace) {
				int j;
				printf("Backtrack:");
				for (j=0; j<st->record_count; j++)
					printf(" (%d,%d)", st->record[j].pos,
					       st->record[j].len);
				printf("\n");
			}
			new = rxl_interp(st, tests[i].form);
			if (!new || strcmp(new, tests[i].replacement) != 0) {
				printf("test %d(%s) replace is <%s>, not <%s>\n",
				       i, patn,
				       new, tests[i].replacement);
				exit(1);
			}
			free(new);
		}
		rxl_free_state(st);
	}
}

int main(int argc, char *argv[])
{
	unsigned short *rxl;
	struct match_state *st;
	int flags;
	enum rxl_found r;
	int len;
	int start;
	int thelen;
	int used;
	int use_file = 0;
	int ccnt = 0;
	int ignore_case = 0;
	int verbatim = 0;
	int opt;
	int trace = False;
	const char *patn, *target, *t;
	char prefix[100];
	int plen;

	while ((opt = getopt(argc, argv, "itvlTf")) > 0)
		switch (opt) {
		case 'f':
			use_file = 1; break;
		case 'i':
			ignore_case = 1; break;
		case 'v':
			verbatim = 1; break;
		case 't':
			trace = True; break;
		case 'T':
			run_tests(trace);
			printf("All tests passed successfully\n");
			exit(0);
		default:
			fprintf(stderr, "Usage: rexel -itvl pattern target\n");
			fprintf(stderr, "     : rexel -itvl -f pattern file\n");
			fprintf(stderr, "     : rexel -T\n");
			exit(1);
		}

	if (optind + 2 != argc) {
		fprintf(stderr,
			"Usage: rexel -ivl pattern target\n"
			"   or: rexel -T\n");
		exit(1);
	}
	patn = argv[optind];
	if (use_file) {
		int fd = open(argv[optind+1], O_RDONLY);
		struct stat stb;
		if (fd < 0) {
			perror(target);
			exit(1);
		}
		fstat(fd, &stb);
		target = mmap(NULL, stb.st_size, PROT_READ, MAP_SHARED, fd, 0);
		close(fd);
	} else
		target = argv[optind+1];

	setlocale(LC_ALL, "");
	setlocale(LC_CTYPE, "enUS.UTF-8");

	if (verbatim)
		rxl = rxl_parse_verbatim(patn, ignore_case);
	else
		rxl = rxl_parse(patn, &len, ignore_case);

	if (!rxl) {
		printf("Failed to parse: %s at %s\n", patn, patn+len);
		exit(2);
	}
	rxl_print(rxl);

	plen = rxl_prefix(rxl, prefix, sizeof(prefix)-1);
	if (plen)
		printf("Prefix: \"%.*s\"\n", plen, prefix);
	else
		printf("No static prefix\n");

	st = rxl_prepare(rxl, 0);
	st->trace = trace;
	t = target;
	flags = RXL_SOL|RXL_SOD;
	do {
		wint_t wc = get_utf8(&t, NULL);
		if (wc >= WERR) {
			rxl_advance(st, RXL_EOL|RXL_EOD);
			break;
		}
		r = rxl_advance(st, wc | flags);
		flags = 0;
		ccnt+= 1;
	} while (r != RXL_DONE);
	rxl_info(st, &thelen, NULL, &start, NULL);
	if (thelen < 0)
		printf("No match\n");
	else {
		int j;
		wint_t wc;
		const char *tstart, *tend;
		tstart = target;
		if (use_file) {
			tstart = target + start;
			while (tstart > target && tstart[-1] != '\n')
				tstart--;
			tend = tstart;
			while (*tend && *tend != '\n')
				tend += 1;
		} else {
			tend = tstart + strlen(tstart);
		}
		printf("%0.*s\n", tend-tstart, tstart);
		t = target;
		ccnt = tstart-target;
		while ((wc = get_utf8(&t, NULL)) < WERR) {
			if (ccnt < start)
				putchar(' ');
			else if (ccnt == start)
				putchar('^');
			else if (ccnt < start + thelen)
				putchar('.');
			ccnt += 1;
			if (t >= tend)
				break;
		}
		putchar('\n');
	}
	rxl_free_state(st);
	exit(0);
}
#endif
