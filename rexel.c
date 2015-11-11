/*
 * Copyright Neil Brown <neil@brown.name> 2015
 * May be distributed under terms of GPLv2 - see file:COPYING
 *
TODO:

=later

 - word breaks etc \b...
 - record where () are when parsing.  A particular ( can be at several places
 - count number of decision points when matching,
 - record maximum number of concurrent paths
 - If have decision points, match should record them in allocated space
 - Follow a decision path to extract substrings for particular () pair.
 - \ lower upper alpha space nonSpace digit wordBoundary...
 - *? lazy: is that possible?  This is only meaningful when collecting the match.
    Maybe we can compare bit-sequences and prefer forward rather than backward.
 - (?| like in perl
 - back references:  need to know what references to expect, and collect them
   (start,len) as we go.
 - \` start of buffer \' end of buffer \= point

 * rexel - A Regular EXpression Evaluation Library
 *    because everyone needs their own regex library
 *
 * This library supports matching without backtracking by providing
 * a single character at a time.  When a match is found, the
 * length of that match is reported.
 *
 * Compiled form of a regex is an array of 16 bit positive numbers called rexels,
 * or Regular EXpression ELements.
 * This involves some cheating as wctype_t (unsigned long int) values
 * are stored in 16 bits.
 * There are three sorts of subarrays, and entry zero is a size of regex array.
 *  A "char class" subarray starts with a count and the 1 or more char
 *    class numbers (squeezed into 16 bytes).
 *  A "char set" subarray.  These have a count and a order list of
 *    char values (or the low 16bits there-of).  If a binary search
 *    finds an even index, that char is in the set,  If it find an odd
 *    index, it isn't.  The top 6 bits of the count are used to match
 *    the bits 16-21 of the char, so only 10bits are available for length.
 *  A "regexp" subarray.  This is a list of numbers with a variety
 *    of meanings.  Each location can represent a point in an ongoing
 *    match.  As a new character is processed, that point might advance
 *    or it might be discarded, or might fork.
 *   The different 'commands' in 4 groups based on first 2 bits.
 *   0x: 15 bit unicode number.  Other unicode numbers cannot be matched
 *           this way
 *   10: address of a "regex" subarray.   The match forks at this point,
 *       both the next entry and the addressed entry are considered.
 *       This limits total size to 4096 entries
 *   11: address of a char set.
 *
 *   The first 4 entries must be in a regexp subarray, or unused, as
 *   0xeffc-0xefff and 0xfffc to 0xffff have special meanings:
 *     0xefff - match any char
 *     0xeffe - match at start of line
 *     0xeffd - match at start of word
 *     0xeffc - reserved
 *     0xffff - match no char - dead end.
 *     0xfffe - match at end of line
 *     0xfffd - match at end of word
 *     0xfffc - report success.
 *
 * When matching, two pairs of extra arrays are allocated and used.
 * One pair is 'before', one pair is 'after'.  They swap on each char.
 * One contains a threaded linkage among all points in regex subarray
 * which are currently matched.  A 'zero' marks the end of the chain.
 * The other records the length of that longest match at that point.
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
 *    some special character classes.  Those classes which are no "everything except"
 *    are permitted equally inside character sets ([]).  The classes are:
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
 */

#include <stdlib.h>
#include <unistd.h>
#include <getopt.h>
#include <wchar.h>
#include <ctype.h>
#include <wctype.h>
#include <memory.h>

#ifdef DEBUG
#include <stdio.h>
#endif
struct match_state {
	unsigned short	*rxl;
	unsigned short	*link[2];
	unsigned short	*leng[2];
	unsigned short	active;
	int		match;
	#ifdef DEBUG
	int		trace;
	#endif
};
#define NO_LINK	0x3FFF
#define	LOOP_CHECK 0x3FFE
#define	REC_ANY		0xBFFF
#define	REC_NONE	0xFFFF
#define	REC_SOL		0xBFFE
#define	REC_EOL		0xFFFE
#define	REC_SOW		0xBFFD
#define	REC_EOW		0xFFFD
#define	REC_MATCH	0xFFFC

#define	REC_FORK	0x8000
#define	REC_SET		0xc000
#define	REC_ISCHAR(x)	(!((x) & 0x8000))
#define	REC_ISSPEC(x)	(!REC_ISCHAR(x) && ((x)&0x3fff) >= 0x3ffc)
#define	REC_ISFORK(x)	(!REC_ISSPEC(x) && ((x) & 0xc000) == REC_FORK)
#define	REC_ISSET(x)	(!REC_ISSPEC(x) && ((x) & 0xc000) == REC_SET)
#define	REC_ADDR(x)	((x) & 0x3fff)

/* First entry contains start of maps, and flags */
#define	RXL_CASELESS		0x8000
#define	RXL_DOTALL		0x4000
#define	RXL_SETSTART(rxl)	((rxl) + ((rxl)[0] & 0x3fff))
#define	RXL_IS_CASELESS(rxl)	((rxl)[0] & RXL_CASELESS)
#define	RXL_IS_DOTALL(rxl)	((rxl)[0] & RXL_DOTALL)

static int classcnt = 0;
static wctype_t *classmap = NULL;

static int do_link(struct match_state *st, int pos, int dest, int len)
{
	unsigned short cmd = st->rxl[pos];
	if (cmd == REC_MATCH) {
		if (st->match < len)
			st->match = len;
		return dest;
	}
	if (!REC_ISFORK(cmd)) {
		/* not a FORK, so just link it in. */
		if (st->link[st->active][pos] == NO_LINK) {
			st->leng[st->active][pos] = len;
			st->link[st->active][dest] = pos;
			st->link[st->active][pos] = 0;
			dest = pos;
		} else if (st->leng[st->active][pos] < len)
			st->leng[st->active][pos] = len;
	} else if (st->link[st->active][pos] == NO_LINK ||
		   st->leng[st->active][pos] < len) {
		st->link[st->active][pos] = LOOP_CHECK;
		st->leng[st->active][pos] = len;
		dest = do_link(st, REC_ADDR(cmd), dest, len);
		dest = do_link(st, pos+1, dest, len);
	}
	return dest;
}

static int set_match(struct match_state *st, unsigned short addr, wchar_t ch)
{
	unsigned short *set = RXL_SETSTART(st->rxl) + addr;
	wchar_t uch = ch, lch = ch;
	unsigned short len;
	int ic = RXL_IS_CASELESS(st->rxl);

	if (ic) {
		/* As Unicode has 3 cases, can we be sure that everything
		 * has a 'lower' to map to?  Surely everything has at least
		 * and upper or a lower...
		 */
		uch = towupper(ch);
		lch = towlower(ch);
	}
	/* First there might be some char classes */
	len = *set++;
	if (len) {
		int invert = len & 0x8000;
		len &= 0x7fff;
		for ( ; len--; set++) {
			if (iswctype(uch, classmap[*set]) ||
			    (uch != lch && iswctype(lch, classmap[*set]))) {
				if (!invert)
					return 1;
			} else {
				if (invert)
					return 1;
			}
		}
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
		hi = len; /* Invar: Every entry at or after hi is greater than target */
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
			return 1;
		set += len;
	}
	/* Didn't find a match anywhere.. */
	return 0;
}


/*
 * Advance the match state to process 'ch' and/or flag.
 * flag indicates start/end of word/line, and whether to ignore ch.
 */
#define	RXL_SOL	1
#define RXL_EOL	2
#define	RXL_SOW	4
#define	RXL_EOW	8
int rxl_advance(struct match_state *st, wint_t ch, int flag, int restart)
{
	int active = st->active;
	int next = 1-active;
	int eol;
	unsigned short i;
	int advance = 0;
	wint_t uch;

	if (RXL_IS_CASELESS(st->rxl)) {
		uch = towupper(ch);
		ch = towlower(ch);
	}

	if (flag && ch != WEOF)
		return -2;
	if (restart) {
		/* If start state is not currently matched, add it with
		 * length of zero
		 */
		eol = 0;
		while (st->link[active][eol])
			eol = st->link[active][eol];
		/* Found the end of the list. */
		do_link(st, 1, eol, 0);
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
		unsigned short cnt;
		int len = RXL_SETSTART(st->rxl) - st->rxl;

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
				} else switch(cmd) {
					case REC_ANY: printf(" .  "); break;
					case REC_NONE:printf(" ## "); break;
					case REC_SOL: printf(" ^  "); break;
					case REC_EOL: printf(" $  "); break;
					case REC_SOW: printf(" \\< "); break;
					case REC_EOW: printf(" \\> "); break;
					case REC_MATCH:printf("!!! "); break;
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
		if (flag)
			printf("Flag: %x\n", flag);
		else
			printf("Match %lc(%x)\n", ch >= ' ' && ch < ' ' ? '?' : ch , ch);

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
	int len = RXL_SETSTART(st->rxl) - st->rxl;
	for (i = 0; i < len; i++) {
		st->link[next][i] = NO_LINK;
		st->leng[next][i] = 0;
	}
	st->link[next][0] = 0;

	/* Now advance each current match */
	for (i = st->link[active][0]; i; i = st->link[active][i]) {
		int len = st->leng[active][i];
		unsigned int cmd = st->rxl[i];

		if (!flag)
			/* If we get a match, then len will have increased */
			len += 1;
		if (REC_ISSPEC(cmd)) {
			switch(cmd) {
			case REC_ANY:
				advance = 1;
				if ((ch == '\n' || ch == '\r' || ch == '\f')
				    && !RXL_IS_DOTALL(st->rxl))
					advance = 0;
				break;
			case REC_MATCH:
				/* cannot match more chars here */
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
			case REC_SOW:
				if (flag & RXL_SOW)
					advance = 1;
				else if (!flag)
					advance = -1;
				else
					advance = 0;
				break;
			case REC_EOW:
				if (flag & RXL_EOW)
					advance = 1;
				else if (!flag)
					advance = -1;
				else
					advance = 0;
				break;
			}
		} else if (flag) {
			/* expecting a char, so ignore position info */
			advance = 0;
		} else if (REC_ISCHAR(cmd)) {
			if (cmd == ch || cmd == uch)
				advance = 1;
			else
				advance = -1;
		} else if (REC_ISSET(cmd)) {
			if (set_match(st, REC_ADDR(cmd), ch))
				advance = 1;
			else
				advance = -1;
		} else
			/* Nothing else is possible here */
			abort();
		if (advance < 0)
			/* no match on this path */
			continue;
		if (advance == 0) {
			/* Nothing conclusive here */
			eol = do_link(st, i, eol, len);
			continue;
		}
		/* Need to advance and link the new address in.
		 * However if there is a fork, we might need to link multiple
		 * addresses in.  Best use recursion.
		 */
		eol = do_link(st, i+1, eol, len);
	}
	st->link[next][eol] = 0;
	if (eol == 0 && st->match < 0)
		return -2;
	return st->match;
}

struct parse_state {
	char	*patn;
	unsigned short	*rxl;
	int	next;
	unsigned short	*sets;
	int	set;	/* Next offset to store a set */
	short	nocase;

	/* Details of set currently being parsed */
	int	invert;
	int	len;
};

static void add_cmd(struct parse_state *st, unsigned short cmd)
{
	if (st->rxl)
		st->rxl[st->next] = cmd;
	st->next += 1;
}

static void relocate(struct parse_state *st, unsigned short start, int len)
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

static int __add_range(struct parse_state *st, wchar_t start, wchar_t end,
		       int plane, int *planes, int *newplane)
{
	int p;
	int lo, hi;
	int len;
	unsigned short *set;
	if (end < start)
		return -1;
	if (!st->sets) {
		/* guess 2 entries for each plane, plus 1
		 * if we add a plane
		 */
		for (p = (start & 0x1F0000)>>16; p <= (end & 0x1F0000)>>16 ; p++) {
			if (!((*planes) & (1 << p))) {
				*planes |= 1 << p;
				st->len += 1;
			}
			st->len += 2;
			if (st->invert)
				st->len += 1;
		}
		*newplane = 0x11 << 16;
		return 0;
	}
	/* OK, for real this time, need to build up set 'plane' */
	if (start >= ((plane+1) << 16)) {
		/* Nothing to do for this plane */
		*newplane = start >> 16;
		return 0;
	}
	if (end < (plane << 16)) {
		/* nothing more to do */
		*newplane = 0x11 << 16;
		return 0;
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
	 *   2a/ if 'start-1' is present, update that end
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
	if (st->invert)
		set += 1;
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
			if (end == 0xffff)
				len = lo-1;
			else
				set[lo-1] = end+1;
		} else if (lo < len && set[lo] <= end)
			set[lo] = start;
		else {
			/* need to insert */
			memmove(set+lo+2, set+lo, sizeof(set[lo])*(len-lo));
			set[lo] = start;
			if (end == 0xffff)
				len = lo+1;
			else {
				set[lo+1] = end+1;
				len+= 2;
			}
		}
	} else {
		/* Already present, lo is end of a range, or beyond len */
		if (lo == len || set[lo] > end)
			/* nothing to do */;
		else
			set[lo] = end+1;
	}
	if (lo)
		lo |= 1;
	else
		lo &= ~1;
	/* Lo now points to the end of a range. If it overlap next,
	 * merge the ranges.
	 */
	/* FIXME not right starting place */
	while (lo+1 < len && set[lo] >= set[lo+1]) {
		if (lo+2 < len){
			if (set[lo] > set[lo+2])
				set[lo+2] = set[lo];
			memmove(set+lo, set+lo+2, len - (lo+2));
		}
		len -= 2;
	}
	if (st->invert)
		len += 1;
	st->len = len;
	return 0;
}

static int add_range(struct parse_state *st, wchar_t start, wchar_t end,
		     int plane, int *planes, int *newplane)
{
	if (!st->nocase ||
	    !iswalpha(start) || !iswalpha(end))
		return __add_range(st, start, end, plane, planes, newplane);
	if (__add_range(st, towlower(start), towlower(end), plane, planes, newplane) < 0)
		return -1;
	return __add_range(st, towupper(start), towupper(end), plane, planes, newplane);
}

static void add_class(struct parse_state *st, int plane, wctype_t cls)
{
	int c;
	if (!st->sets) {
		/* one entry required per class */
		st->len += 1;
		return;
	} else if (plane > 0)
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


static int is_set_element(char *p)
{
	int i;
	if (*p != '[')
		return 0;
	if (p[1] != '.' && p[1] != '=' && p[1] != ':')
		return 0;
	for (i = 2; p[i]; i++)
		if (p[i] == ']') {
			if (p[i-1] == p[1] && i > 2)
				return 1;
			else
				return 0;
		}
	return 0;
}

/* FIXME UNICODE */
static int do_parse_set(struct parse_state *st, int plane)
{
	mbstate_t ps = {0};
	char *p = st->patn;
	wchar_t ch;
	int newplane = 0xFFFFFF;
	int planes = 0;
	/* first characters are special... */
	st->invert = 0;
	st->len = 0;
	if (*p == '^') {
		st->invert = 1;
		p += 1;
	}
	do {
		int l = mbrtowc(&ch, p, 5, &ps);
		if (ch == '[' && is_set_element(p)) {
			switch(p[1]) {
			case '.': /* collating set */
			case '=': /* collating element */
				st->patn = p+1;
				return -1;
			case ':': /* character class */
			{
				char *e, *cls;
				wctype_t wct;
				p += 2;
				e = strchr(p, ':');
				cls = strndup(p, e-p);
				wct = wctype(cls);
				free(cls);
				if (!wct)
					return -1;
				p = e;
				while (*p != ']')
					p++;
				p++;
				add_class(st, plane, wct);
				break;
			}
			}
		} else if (l && p[l] == '-' && p[l+1] != ']') {
			/* range */
			wchar_t ch2;
			l += mbrtowc(&ch2, p+l+1, 5, &ps);
			if (add_range(st, ch, ch2,
				      plane, &planes, &newplane) < 0)
				return -1;

			p += l+1;
		} else if (ch == '\\' && p[1] > 0 && p[1] < 0x7f && p[2] != '-'
			   && strchr("daApsw", p[1]) != NULL) {
			switch (p[1]) {
			case 'd': add_class(st, plane, wctype("digit")); break;
			case 'a': add_class(st, plane, wctype("lower")); break;
			case 'A': add_class(st, plane, wctype("upper")); break;
			case 'p': add_class(st, plane, wctype("punct")); break;
			case 's': add_class(st, plane, wctype("space")); break;
			case 'w': add_class(st, plane, wctype("alpha")); break;
			}
			p += 2;
		} else if (ch) {
			if (add_range(st, ch, ch,
				      plane, &planes, &newplane) < 0)
				return -1;
			p += l;
		} else
			return -1;
	} while (*p != ']');
	st->patn = p+1;
	if (st->sets) {
		if (plane < 0) {
			/* We have a (possibly empty) class list. Record size */
			unsigned short l = st->len;
			if (l && st->invert)
				l |= 0x8000;
			st->sets[st->set] = l;
		} else {
			/* We have a set, not empty.  Store size and leading zero
			 * if inverted */
			unsigned short l = st->len;
			st->sets[st->set] = l;
			if (st->invert)
				st->sets[st->set + 1] = 0;
		}
	}
	st->set += st->len+1;
	return newplane;
}

static int parse_set(struct parse_state *st)
{
	int plane;
	char *patn;
	int set;

	if (*st->patn++ != '[')
		return 0;
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
		return 0;
	if (st->sets)
		st->sets[st->set] = 0;
	st->set++;
	add_cmd(st, REC_SET | set);
	return 1;
}

int cvt_hex(char *s, int len)
{
	long rv = 0;
	while (len) {
		if (!*s || !isxdigit(*s))
			return -1;
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

static unsigned short  add_class_set(struct parse_state *st, char *cls, int in)
{
	if (!st->rxl) {
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
static int parse_re(struct parse_state *st);
static int parse_atom(struct parse_state *st)
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
	 * If there is a syntax error, return 0, else return 1;
	 *
	 */
	wchar_t ch;
	/* Only '.', (), and char for now */

	if (*st->patn == '\0')
		return 0;
	if (*st->patn == '.') {
		add_cmd(st, REC_ANY);
		st->patn++;
		return 1;
	}
	if (*st->patn == '(') {
		st->patn++;
		if (!parse_re(st))
			return 0;
		if (*st->patn != ')')
			return 0;
		st->patn++;
		return 1;
	}
	if (*st->patn == '^') {
		add_cmd(st, REC_SOL);
		st->patn++;
		return 1;
	}
	if (*st->patn == '$') {
		add_cmd(st, REC_EOL);
		st->patn++;
		return 1;
	}
	if (*st->patn == '[')
		return parse_set(st);
	if (*st->patn & 0x80) {
		mbstate_t ps = {0};
		int len = mbrtowc(&ch, st->patn, 5, &ps);
		if (len <= 0)
			return 0;
		st->patn += len-1;
	} else
		ch = *st->patn;
	if (ch == '\\') {
		st->patn++;
		ch = *st->patn;
		switch (ch) {
			/* These just fall through are are interpreted literally */
		case '^':
		case '.':
		case '[':
		case '$':
		case '(':
		case ')':
		case '|':
		case '*':
		case '+':
		case '?':
		case '{':
		case '\\':
			break;
			/* These are simple translations */
		case '<': ch = REC_SOW; break;
		case '>': ch = REC_EOW; break;
		case 't': ch = '\t'; break;
		case 'n': ch = '\n'; break;
		case '0': ch = 0;
			while (st->patn[1] >= '0' && st->patn[1] <= '7') {
				ch = ch*8 + st->patn[1] - '0';
				st->patn+= 1;
			}
			break;
		case 'x': ch = cvt_hex(st->patn+1, 2);
			if (ch < 0)
				return 0;
			st->patn += 2;
			break;
		case 'u': ch = cvt_hex(st->patn+1, 4);
			if (ch < 0)
				return 0;
			st->patn += 4;
			break;
		case 'U': ch = cvt_hex(st->patn+1, 8);
			if (ch < 0)
				return 0;
			st->patn += 8;
			break;
			/* Anything else is an error (e.g. \0) or
			 * reserved for future use.
			 */
		case 'd': ch = add_class_set(st, "digit", 1); break;
		case 'D': ch = add_class_set(st, "digit", 0); break;
		case 's': ch = add_class_set(st, "space", 1); break;
		case 'S': ch = add_class_set(st, "space", 0); break;
		case 'w': ch = add_class_set(st, "alpha", 1); break;
		case 'W': ch = add_class_set(st, "alpha", 0); break;
		case 'p': ch = add_class_set(st, "punct", 1); break;
		case 'P': ch = add_class_set(st, "punct", 0); break;

		case 'a': ch = add_class_set(st, "lower", 1); break;
		case 'A': ch = add_class_set(st, "upper", 0); break;

		default: return 0;
		}
	}
	add_cmd(st, ch);
	st->patn++;
	return 1;
}

static int parse_piece(struct parse_state *st)
{
	int start = st->next;
	char c;
	int min, max;
	char *ep;
	int skip;

	if (!parse_atom(st))
		return 0;
	c = *st->patn;
	if (c != '*' && c != '+' && c != '?' &&
	    !(c=='{' && isdigit(st->patn[1])))
		return 1;

	st->patn++;
	switch(c) {
	case '*':
		/* make spare for 'jump forward' */
		relocate(st, start, 1);
		/* 'jump_backward */
		add_cmd(st, REC_FORK | (start+1));
		if (st->rxl)
			st->rxl[start] = REC_FORK | st->next;
		return 1;
	case '+':
		/* just (optional) jump back */
		add_cmd(st, REC_FORK | start);
		return 1;
	case '?':
		/* Just a jump-forward */
		relocate(st, start, 1);
		if (st->rxl)
			st->rxl[start] = REC_FORK | st->next;
		return 1;
	case '{':/* Need a number, maybe a comma, if no maybe a number, then } */
		min = strtoul(st->patn, &ep, 10);
		if (min > 256)
			return 0;
		max = min;
		if (*ep == ',') {
			max = -1;
			ep++;
			if (isdigit(*ep)) {
				max = strtoul(ep, &ep, 10);
				if (max > 256 || max < min)
					return 0;
			}
		}
		if (*ep != '}')
			return 0;
		st->patn = ep+1;
		/* Atom need to be repeated min times, and maybe as many as 'max',
		 * or indefinitely if max < 0
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
				st->rxl[start] = REC_FORK | st->next;
				skip = start;
			}
			start += 1;
		}
		if (max < 0) {
			add_cmd(st, REC_FORK | start);
		} else if (max > 1) {
			/* need to duplicate atom but make each one optional */
			int len = st->next - start;
			int last = st->next + (len + 1) * (max-1);
			if (skip && st->rxl) {
				st->rxl[skip] = REC_FORK | last;
			}
			while (max > 1) {
				int newstart;
				add_cmd(st, REC_FORK | last);
				newstart = st->next;
				relocate(st, start, len+1);
				st->next -= 1;
				start = newstart;
				max -= 1;
			}
			if (last != st->next)
				abort();
		}
		return 1;
	}
	return 0;
}

static int parse_branch(struct parse_state *st)
{
	do {
		if (!parse_piece(st))
			return 0;
		switch (*st->patn) {
		case '*':
		case '+':
		case '?':
			/* repeated modifier - illegal */
			return 0;
		}
	} while (*st->patn && *st->patn != '|' && *st->patn != ')');
	return 1;
}

static int parse_re(struct parse_state *st)
{
	int start = st->next;
	if (!parse_branch(st))
		return 0;
	if (*st->patn != '|')
		return 1;
	st->patn += 1;
	relocate(st, start, 1);
	if (st->rxl)
		st->rxl[start] = REC_FORK | (st->next + 2);
	start = st->next;
	add_cmd(st, REC_NONE); /* will become 'jump to end' */
	add_cmd(st, REC_NONE);
	if (parse_re(st) == 0)
		return 0;
	if (st->rxl)
		st->rxl[start] = REC_FORK | st->next;
	return 1;
}


unsigned short *rxl_parse(char *patn, int *lenp, int nocase)
{
	struct parse_state st;
	st.patn = patn;
	st.nocase = nocase;
	st.rxl = NULL;
	st.next = 1;
	st.sets = NULL;
	st.set = 0;
	if (parse_re(&st) == 0) {
		if (lenp)
			*lenp = st.patn - patn;
		return NULL;
	}
	add_cmd(&st, REC_MATCH);
	st.rxl = malloc((st.next + st.set) * sizeof(st.rxl[0]));
	st.rxl[0] = st.next;
	if (nocase)
		st.rxl[0] |= RXL_CASELESS;
	st.sets = st.rxl + st.next;
	st.patn = patn;
	st.next = 1;
	st.set = 0;
	if (parse_re(&st) == 0)
		abort();
	add_cmd(&st, REC_MATCH);
	return st.rxl;
}

unsigned short *rxl_parse_verbatim(char *patn, int nocase)
{
	struct parse_state st;
	int i, l;
	mbstate_t ps = {0};
	wchar_t ch;

	st.next = 1 + strlen(patn) + 1;
	st.rxl = malloc(st.next * sizeof(st.rxl[0]));
	st.rxl[0] = st.next;
	if (nocase)
		st.rxl[0] |= RXL_CASELESS;
	st.next = 1;
	for (i = 0; (l = mbrtowc(&ch, patn+i, 5, &ps)) != 0; i += l)
		add_cmd(&st, ch);
	add_cmd(&st, REC_MATCH);
	return st.rxl;
}

static void setup_match(struct match_state *st, unsigned short *rxl)
{
	int len = RXL_SETSTART(rxl) - rxl;
	int i;

	st->rxl = rxl;
	st->link[0] = malloc(len * sizeof(unsigned short) * 4);
	st->link[1] = st->link[0] + len;
	st->leng[0] = st->link[1] + len;
	st->leng[1] = st->leng[0] + len;
	st->active = 0;
	st->match = -1;
	for (i = 0; i < len; i++) {
		st->link[0][i] = NO_LINK;
		st->link[1][i] = NO_LINK;
	}
	/* The list of states is empty */
	st->link[1-st->active][0] = 0;
	st->link[st->active][0] = 0;
}

void *rxl_prepare(unsigned short *rxl)
{
	struct match_state *ret;

	ret = malloc(sizeof(*ret));
	setup_match(ret, rxl);
	return ret;
}

void rxl_free_state(void *v)
{
	struct match_state *s = v;

	free(s->link[0]);
	free(s);
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

static void print_set(unsigned short *set)
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

void rxl_print(unsigned short *rxl)
{
	unsigned short *set = RXL_SETSTART(rxl);
	unsigned short *i;

	for (i = rxl+1 ; i < set; i++) {
		unsigned short cmd = *i;
		printf("%04u: ", i-rxl);
		if (REC_ISCHAR(cmd))
			printf("match %lc (#%x)\n", cmd, cmd);
		else if (REC_ISSPEC(cmd)) {
			switch(cmd) {
			case REC_ANY: printf("match ANY\n"); break;
			case REC_NONE: printf("DEAD END\n"); break;
			case REC_SOL: printf("match start-of-line\n"); break;
			case REC_EOL: printf("match end-of-line\n"); break;
			case REC_SOW: printf("match start-of-word\n"); break;
			case REC_EOW: printf("match end-of-word\n"); break;
			case REC_MATCH: printf("MATCHING COMPLETE\n"); break;
			default: printf("ERROR %x\n", cmd); break;
			}
		} else if (REC_ISFORK(cmd))
			printf("branch to %d\n", REC_ADDR(cmd));
		else if (REC_ISSET(cmd)) {
			printf("Match from set %d: ", REC_ADDR(cmd));
			print_set(set + REC_ADDR(cmd));
			printf("\n");
		} else
			printf("ERROR %x\n", cmd);
	}
}

enum {
	F_VERB = 1,
	F_ICASE = 2,
	F_PERR = 4,
};
static struct test {
	char *patn, *target;
	int flags, start, len;
} tests[] = {
	{ "abc", "the abc", 0, 4, 3},
	{ "a*", " aaaaac", 0, 1,  5},
};
static void run_tests()
{
	int cnt = sizeof(tests) / sizeof(tests[0]);
	int i;

	for (i = 0; i < cnt; i++) {
		int f = tests[i].flags;
		char *patn = tests[i].patn;
		char *target = tests[i].target;
		unsigned short *rxl;
		int mstart = -1, mlen;
		int len, ccnt = 0;

		mbstate_t ps = {0};
		struct match_state st = {0};

		if (f & F_VERB)
			rxl = rxl_parse_verbatim(patn, f & F_ICASE);
		else
			rxl = rxl_parse(patn, &len, f & F_ICASE);
		if (!rxl && !(f & F_PERR)) {
			printf("test %d: Parse error at %d\n", i, len);
			exit(1);
		} else if (f & F_PERR) {
			printf("test %d: No parse error found\n", i);
			exit (1);
		}
		setup_match(&st, rxl);

		mstart = -1;
		rxl_advance(&st, WEOF, RXL_SOL, 1);
		while (mstart < 0 || len > 0) {
			wchar_t wc;
			int used = mbrtowc(&wc, target, 5, &ps);
			if (used <= 0)
				break;
			len = rxl_advance(&st, wc, 0, mstart < 0);
			target += used;
			ccnt += 1;
			if (len >= 0 &&
			    (mstart < 0  || ccnt-len < mstart ||
			     (ccnt-len) == mstart && len > mlen)) {
				mstart = ccnt - len;
				mlen = len;
			}
		}
		if (*target == 0) {
			len = rxl_advance(&st, WEOF, RXL_EOL, mstart < 0);
			if (mstart < 0 && len >= 0) {
				mstart = ccnt - len;
				mlen = len;
			}
		}
		if (tests[i].start != mstart ||
		    tests[i].len != mlen) {
			printf("test %d: found %d/%d instead of %d/%d\n", i,
			       mstart, mlen, tests[i].start, tests[i].len);
			exit(1);
		}
	}
}

int main(int argc, char *argv[])
{
	unsigned short *rxl;
	struct match_state st;
	int len;
	int i;
	int start;
	int thelen;
	int used;
	int ccnt = 0;
	int ignore_case = 0;
	int verbatim = 0;
	int longest = 0;
	int opt;
	mbstate_t ps = {0};
	char *patn, *target;

	while ((opt = getopt(argc, argv, "ivlT")) > 0)
		switch (opt) {
		case 'i':
			ignore_case = 1; break;
		case 'v':
			verbatim = 1; break;
		case 'l':
			longest = 1; break;
		case 'T':
			run_tests();
			printf("All tests passed successfully\n");
			exit(0);
		default:
			fprintf(stderr, "Usage: rexel -ivl pattern target\n   or: rexel -T\n");
			exit(1);
		}

	if (optind + 2 != argc) {
		fprintf(stderr, "Usage: rexel -ivl pattern target\n   or: rexel -T\n");
		exit(1);
	}
	patn = argv[optind];
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

	setup_match(&st, rxl);
	#ifdef DEBUG
	st.trace = 1;
	#endif
	i = 0;
	len = -1;
	rxl_advance(&st, WEOF, RXL_SOL, 1);
	while (len < 0) {
		wchar_t wc;
		used = mbrtowc(&wc, target+i, 5, &ps);
		if (used <= 0)
			break;
		len = rxl_advance(&st, wc, 0, 1);
		i+= used;
		ccnt+= 1;
	}
	/* We have a match, let's see if we can extend it */
	start = ccnt-len; thelen = len;
	if (len >= 0) {
		while (len != -2 || longest) {
			wchar_t wc;
			used = mbrtowc(&wc, target+i, 5, &ps);
			if (used <= 0)
				break;
			len = rxl_advance(&st, wc, 0, longest);
			i += used;
			ccnt += 1;
			if (longest) {
				if (len > thelen) {
					start = ccnt - len;
					thelen = len;
				}
			} else {
				if (ccnt-len < start ||
				    (ccnt-len) == start && len > thelen) {
					start = ccnt-len;
					thelen = len;
				}
			}
		}
		if (target[i] == 0)
			len = rxl_advance(&st, WEOF, RXL_EOL, longest);
	}
	if (thelen < 0)
		printf("No match\n");
	else {
		int j;
		wchar_t wc;
		printf("%s\n", target);
		memset(&ps, 0, sizeof(ps));
		i = 0;
		ccnt = 0;
		while ((used = mbrtowc(&wc, target+i, 5, &ps)) > 0) {
			if (ccnt < start)
				putchar(' ');
			else if (ccnt == start)
				putchar('^');
			else if (ccnt < start + thelen)
				putchar('.');
			i+= used;
			ccnt += 1;
		}
		putchar('\n');
	}
	exit(0);
}
#endif
