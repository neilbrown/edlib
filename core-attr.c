/*
 * Copyright Neil Brown ©2015-2020 <neil@brown.name>
 * May be distributed under terms of GPLv2 - see file:COPYING
 *
 * Attributes.
 *
 * Attributes are attached to text in buffers and to marks and probably
 * other things.
 * They are simply name=value pairs, stored as strings though direct
 * conversion to numbers and Bools is provided.
 * Values must be "small".  The name and value together must be less than
 * 512 bytes, and there is probably some padding in there.  If you get
 * even close to this limit you are doing something wrong.
 * Larger strings need to be stored elsewhere with some sort of indirect.
 * (Hmmm.. this excludes file names - is that a good idea?)
 *
 * Attributes are stored in a list sorted by attribute name.  Strings
 * of digits in the name sort like the number they represent, so "6hello"
 * comes before "10world".  When such a number compares against a single
 * non-digit character the char comes first.
 *
 * Attributes for text are stored in one list for a section of text.
 * Each attribute is prefixed by the offset where the attribute applies.
 *
 * The offsets are really byte offsets - the text is utf-8.
 *
 * When attributes are stored on non-text objects they don't have
 * a number prefix.
 *
 */

#include <stdlib.h>
#include <malloc.h>
#include <string.h>
#include <ctype.h>
#include "core.h"

struct attrset {
	unsigned short	size, /* space allocated */
			len;  /* space used */
	struct attrset *next;
	char		attrs[0];
};

#if defined(TEST_ATTR_ADD_DEL)
#define MAX_ATTR_SIZE (64 - sizeof(struct attrset))
#else
#define MAX_ATTR_SIZE (512 - sizeof(struct attrset))
#endif

static struct attrset *safe newattr(struct attrset *old, int size)
{
	struct attrset *set = realloc(old, sizeof(struct attrset) + size);
	set->size = size;
	if (old == NULL) {
		set->len = 0;
		set->next = NULL;
	}
	return set;
}

/* attr_cmp just deals with bytes and ASCII digits, so it is
 * not aware for wchars
 */
static int getcmptok(const char **ap safe)
{
	const char *a safe;
	char c;
	int i;

	if (!*ap)
		/* FIXME smatch should handle "char * safe *ap safe" */
		return 0;
	a = *ap;
	c = *a++;
	if (!isdigit(c)) {
		*ap = a;
		return c;
	}
	i = c - '0';
	while (isdigit(*a)) {
		c = *a++;
		i = i*10 + (c - '0');
	}
	*ap = a;
	return i+256;
}

/* Compare 'a' and 'b' treating strings of digits as numbers.
 * If bnum >= 0, it is used as a leading number on 'b'.
 */
static int attr_cmp(const char *a safe, const char *b safe, int bnum)
{
	while (*a && (*b || bnum >= 0)) {
		int ai, bi;
		ai = getcmptok(&a);
		if (bnum >= 0) {
			bi = bnum + 256;
			bnum = -1;
			if (*a == ' ')
				a++;
		} else
			bi = getcmptok(&b);
		if (ai < bi)
			return -1;
		if (ai > bi)
			return 1;
	}
	if (*a)
		return 1;
	if (*b)
		return -1;
	return 0;
}

#ifdef TEST_ATTR_CMP
#include <stdlib.h>
#include <stdio.h>
struct {const char *a, *b; int result;} test[] = {
	{ "hello", "there", -1},
	{ "6hello", "10world", -1},
	{ "0005six", "5six", 0},
	{ "ab56", "abc", 1},
};
int main(int argc, char *argv[])
{
	int i;
	int rv = 0;
	for (i = 0; i < sizeof(test)/sizeof(test[0]); i++) {
		if (attr_cmp(test[i].a, test[i].b, -1) == test[i].result)
			printf("%s <-> %s = %d OK\n",
			       test[i].a, test[i].b, test[i].result);
		else {
			printf("%s <-> %s = %d, not %d\n",
			       test[i].a, test[i].b, attr_cmp(test[i].a,
							      test[i].b, -1),
			       test[i].result);
			rv = 1;
		}
	}
	exit(rv);
}

#endif

static int __attr_find(struct attrset ***setpp safe, const char *key safe,
		       int *offsetp safe, int keynum)
{
	struct attrset **setp safe;
	struct attrset *set;
	int i;

	if (!*setpp)
		/* FIXME smatch should check this */
		return -1;
	setp = *setpp;
	set = *setp;
	if (!set)
		return -1;
	*offsetp = 0;
	while (set->next &&
	       attr_cmp(set->next->attrs, key, keynum) <= 0) {
		setp = &set->next;
		set = *setp;
	}
	*setpp = setp;

	for (i = 0; i < set->len; ) {
		int cmp = attr_cmp(set->attrs + i, key, keynum);
		if (cmp >= 0) {
			*offsetp = i;
			*setpp = setp;
			return cmp;
		}
		i += strlen(set->attrs + i) + 1;
		i += strlen(set->attrs + i) + 1;
	}
	*offsetp = i;
	return 1;
}

static void do_del(struct attrset * *setp safe, int offset)
{
	int len;
	struct attrset *set;

	set = safe_cast *setp;
	len = strlen(set->attrs + offset) + 1;
	len += strlen(set->attrs + offset + len) + 1;
	memmove(set->attrs + offset,
		set->attrs + offset + len,
		set->len - (offset + len));
	set->len -= len;
	if (set->len == 0) {
		*setp = set->next;
		free(set);
	}
}

bool attr_del(struct attrset * *setp safe, const char *key safe)
{
	int offset = 0;
	int cmp;

	cmp = __attr_find(&setp, key, &offset, -1);

	if (cmp)
		/* Not found */
		return False;
	do_del(setp, offset);
	return True;
}

void attr_del_all(struct attrset * *setp safe, const char *key safe,
		  int low, int high)
{
	int offset = 0;
	/* Delete all attrs 'key' with keynum from 'low' to 'high' */
	while (low <= high) {
		struct attrset *set;
		int n;
		int cmp = __attr_find(&setp, key, &offset, low);

		if (cmp < 0)
			/* Nothing more to find */
			return;
		low += 1;
		if (cmp == 0) {
			/* Found, better delete */
			do_del(setp, offset);
			continue;
		}
		/* found something higher, possibly update 'low'
		 * to skip over gaps.
		 */
		set = *setp;
		if (!set || offset >= set->len)
			continue;
		n = atoi(set->attrs + offset);
		if (n > low)
			low = n;
	}
}

char *attr_get_str(struct attrset *set, const char *key safe, int keynum)
{
	struct attrset **setp = &set;
	int offset = 0;
	int cmp = __attr_find(&setp, key, &offset, keynum);

	if (cmp != 0 || !*setp)
		return NULL;
	set = *setp;
	offset += strlen(set->attrs + offset) + 1;
	return set->attrs + offset;
}

char *attr_find(struct attrset *set, const char *key safe)
{
	return attr_get_str(set, key, -1);
}

const char *attr_get_next_key(struct attrset *set, const char *key safe,
			      int keynum, const char **valp safe)
{
	struct attrset **setp = &set;
	int offset = 0;
	int cmp = __attr_find(&setp, key, &offset, keynum);
	const char *val;

	if (cmp < 0)
		return NULL;
	set = safe_cast *setp;
	if (cmp == 0) {
		/* Skip the matching key, then value */
		offset += strlen(set->attrs + offset) + 1;
		offset += strlen(set->attrs + offset) + 1;
	}
	if (offset >= set->len) {
		set = set->next;
		offset = 0;
	}
	if (!set)
		return NULL;
	key = set->attrs + offset;
	val = key + strlen(key) + 1;
	if (keynum >= 0) {
		int kn = getcmptok(&key);
		if (kn != keynum + 256)
			return NULL;
		if (*key == ' ')
			key += 1;
	}
	*valp = val;
	return key;
}

int attr_set_str_key(struct attrset **setp safe,
		     const char *key safe, const char *val,
		     int keynum)
{
	int offset = 0;
	int cmp;
	struct attrset *set;
	unsigned int len;
	char nkey[22];
	int nkeylen = 0;

	cmp = __attr_find(&setp, key, &offset, keynum);

	if (cmp == 0)
		/* Remove old value */
		do_del(setp, offset);

	if (!val)
		return cmp;

	set = *setp;
	if (keynum >= 0) {
		snprintf(nkey, sizeof(nkey), "%d ", keynum);
		nkeylen = strlen(nkey);
	} else
		nkey[0] = 0;
	len = nkeylen + strlen(key) + 1 + strlen(val) + 1;
	if (set == NULL || set->len + len > set->size) {
		/* Need to re-alloc or alloc new */
		if (!set) {
			set = newattr(NULL, len);
			*setp = set;
		} else if (set->len + len <= MAX_ATTR_SIZE) {
			/* Just make this block bigger */
			set = newattr(set, set->len + len);
			*setp = set;
		} else if (offset + len <= MAX_ATTR_SIZE) {
			/* split following entries in separate block */
			struct attrset *new = newattr(NULL, set->len - offset);

			new->next = set->next;
			set->next = new;
			new->len = set->len - offset;
			set->len = offset;
			memcpy(new->attrs, set->attrs + offset,
			       new->len);
		} else {
			/* Split follow in entries and store new entry there */
			struct attrset *new = newattr(NULL,
						      set->len - offset + len);

			new->next = set->next;
			set->next = new;
			new->len = set->len - offset;
			set->len = offset;
			memcpy(new->attrs, set->attrs + offset,
			       new->len);
			set = new;
			offset = 0;
		}
	}
	memmove(set->attrs + offset + len, set->attrs + offset,
		set->len - offset);
#pragma GCC diagnostic push
// GCC really doesn't like the games I"m playing here.
#pragma GCC diagnostic ignored "-Wstringop-overflow"
	strcpy(set->attrs + offset, nkey);
	strcpy(set->attrs + offset + nkeylen, key);
	strcpy(set->attrs + offset + nkeylen + strlen(key) + 1, val);
#pragma GCC diagnostic pop
	set->len += len;
	return cmp;
}

int attr_set_str(struct attrset **setp safe,
		 const char *key safe, const char *val)
{
	return attr_set_str_key(setp, key, val, -1);
}

#if defined(TEST_ATTR_ADD_DEL) || defined(TEST_ATTR_TRIM)
void attr_dump(struct attrset *set)
{
	printf("DUMP ATTRS:\n");
	while (set) {
		int i;
		printf(" %d of %d:\n", set->len, set->size);
		for (i = 0; i < set->len; ) {
			printf("  %3d: \"%s\"", i, set->attrs + i);
			i += strlen(set->attrs+i) + 1;
			printf(" -> \"%s\"\n", set->attrs + i);
			i += strlen(set->attrs+i) + 1;
		}
		set = set->next;
	}
	printf("END DUMP\n");
}
#endif

#ifdef TEST_ATTR_ADD_DEL
enum act { Add, Remove, Find };
struct action {
	enum act act;
	char *key;
	char *val;
} actions[] = {
	{ Add, "Hello", "world"},
	{ Add, "05 Foo", "Bar" },
	{ Add, "1 Bold", "off" },
	{ Add, "9 Underline", "on" },
	{ Remove, "Hello", NULL },
	{ Find, "5 Foo", "Bar" },
	{ Add, "20 Thing", "Stuff" },
	{ Add, "01 Bold", "on" },
	{ Add, "1 StrikeThrough", "no" },
	{ Add, "2 StrikeThrough", "no" },
	{ Find, "1 StrikeThrough", "no" },
	{ Find, "5 Foo", "Bar" },
	{ Add, "1 Nextthing", "nonono" },
};

int main(int argc, char *argv[])
{
	int i;
	int rv = 0;
	struct attrset *set = NULL;
	for (i = 0; i < sizeof(actions)/sizeof(actions[0]); i++) {
		struct action *a = &actions[i];
		char *v;
		switch(a->act) {
		case Add:
			attr_set_str(&set, a->key, a->val); continue;
		case Remove:
			if (!attr_del(&set, a->key)) {
				printf("Action %d: Remove %s: failed\n",
				       i, a->key);
				rv = 1;
				break;
			}
			continue;
		case Find:
			v = attr_find(set, a->key);
			if (!v || strcmp(v, a->val) != 0) {
				printf("Action %d: Find %s: Found %s\n",
				       i, a->key, v);
				rv = 1;
				break;
			}
			continue;
		}
		break;
	}
	attr_dump(set);
	exit(rv);
}
#endif

/* Have versions that take and return numbers, '-1' for 'not found' */
int attr_find_int(struct attrset *set, const char *key safe)
{
	const char *val = attr_find(set, key);
	unsigned long rv;
	char *end;

	if (!val)
		return -1;
	rv = strtoul(val, &end, 10);
	if (!end || end == val || *end)
		return -1;
	return rv;
}

int attr_set_int(struct attrset **setp safe, const char *key safe, int val)
{
	/* 3 digits per bytes, +1 for sign and +1 for trailing nul */
	char sval[sizeof(int)*3+2];

	snprintf(sval, sizeof(sval), "%d", val);
	return attr_set_str(setp, key, sval);
}

#ifdef TEST_ATTR_INT
int main(int argc, char *argv[])
{
	struct attrset *set = NULL;

	attr_set_int(&set, "One", 1);
	attr_set_int(&set, "Twelve", 12);
	attr_set_int(&set, "Four", 4);
	if (attr_find_int(set, "One") +
	    attr_find_int(set, "Twelve") +
	    attr_find_int(set, "Four")
	    != 17) {
		printf("Didn't find One, Twelve, Four\n");
		exit(2);
	}
	if (attr_find_int(set, "Three") != -1) {
		printf("Surprisingly found Three\n");
		exit(2);
	}
	exit(0);
}
#endif

void attr_free(struct attrset **setp safe)
{
	struct attrset *set = *setp;
	*setp = NULL;
	while (set) {
		struct attrset *next = set->next;
		free(set);
		set = next;
	}
}

/*
 * When attributes are attached to a section of text,
 * we might want to split the text and so split the attributes.
 * So:
 * 1- 'trim' all attributes beyond a given key
 * 2- copy attributes, subtracting some number from the offset.
 *     At offset '0', an empty value deletes the key.
 */

void attr_trim(struct attrset **setp safe, int nkey)
{
	int offset;
	struct attrset *set;

	__attr_find(&setp, "", &offset, nkey);
	set = *setp;
	if (!set)
		return;
	set->len = offset;
	if (offset)
		setp = &set->next;
	attr_free(setp);
}

/* make a copy of 'set', keeping only attributes from 'nkey'
 * onwards.
 */
struct attrset *attr_copy_tail(struct attrset *set, int nkey)
{
	struct attrset *newset = NULL;

	for (; set ; set = set->next) {
		int i;
		for (i = 0; i < set->len; ) {
			char *k, *v;
			int n;
			k = set->attrs + i;
			i += strlen(k) + 1;
			v = set->attrs + i;
			i += strlen(v) + 1;
			n = atoi(k);
			if (n < nkey)
				continue;
			while (*k && *k != ' ')
				k++;
			if (*k == ' ')
				k++;

			attr_set_str_key(&newset, k, v, n);
		}
	}

	return newset;
}

/* Collect the attributes in effect at a given pos and return
 * a new set with the new alternate numeric prefix, or nothing if '-1'
 */
struct attrset *attr_collect(struct attrset *set, unsigned int pos,
			     int prefix)
{
	struct attrset *newset = NULL;
	char kbuf[512];

	for (; set ; set = set->next) {
		int i;
		for (i = 0; i < set->len; ) {
			char *k, *v, *e;
			unsigned long n;
			k = set->attrs + i;
			i += strlen(k) + 1;
			v = set->attrs + i;
			i += strlen(v) + 1;
			n = strtoul(k, &e, 10);

			if (n > pos)
				goto done;
			/* FIXME shouldn't need to test 'e' */
			while (e && *e == ' ')
				e++;
			if (prefix >= 0) {
				snprintf(kbuf, 512, "%d %s", prefix, e);
				e = kbuf;
			}
			if (*v == '\0')
				v = NULL;
			if (!e) e = "FIXME";
			attr_set_str(&newset, e, v);
		}
	}
done:
	return newset;
}

#ifdef TEST_ATTR_TRIM

char *keys[] = {
	"1 Bold", "2 Bold", "5 Bold", "10 Bold",
	"0 Colour", "3 Colour", "08 Colour", "12 Colour",
	"2 Invis", "4 Invis", "6 Invis", "9 Invis",
};

int main(int argc, char *argv[])
{
	int i;
	struct attrset *set = NULL;
	struct attrset *newset, *new2;

	for (i = 0; i < sizeof(keys)/sizeof(keys[0]); i++)
		attr_set_str(&set, keys[i], keys[i], -1);

	newset = attr_copy_tail(set, 5);
	attr_trim(&set, 5);
	new2 = attr_collect(newset, 9, 4);
	attr_dump(set);
	attr_dump(newset);
	attr_dump(new2);
	exit(0);
}
#endif

/*
 * Iterator for set.
 */
