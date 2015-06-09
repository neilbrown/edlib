/*
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
 * Each attribute is prefixed by the offset where the change takes place.
 * So:
 *   "0 bold=true, 0 underline=false, 5 underline=true, 6 bold=false"
 * means that the first 5 chars are bold, not underline.  The next is
 * bold and underline, subsequent chars are underline but not bold.
 * The section of text has a 'start' and 'end'.  Offsets are in that
 * range and so may not start at zero.
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

static struct attrset *newattr(struct attrset *old, int size)
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
static int getcmptok(char **ap)
{
	char *a = *ap;
	char c = *a++;
	int i;
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
 * Any leading numbers less that 'min' are treated as though
 * they were 'min'.
 */
static int attr_cmp(char *a, char *b, int min)
{

	while (*a && *b) {
		int ai, bi;
		ai = getcmptok(&a);
		bi = getcmptok(&b);
		if (ai >= 256 && ai < min+256)
			ai = min + 256;
		if (bi >= 256 && bi < min+256)
			bi = min + 256;
		if (ai < bi)
			return -1;
		if (ai > bi)
			return 1;
		min = 0;
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
struct {char *a, *b; int result;} test[] = {
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
		if (attr_cmp(test[i].a, test[i].b, 0) == test[i].result)
			printf("%s <-> %s = %d OK\n",
			       test[i].a, test[i].b, test[i].result);
		else {
			printf("%s <-> %s = %d, not %d\n",
			       test[i].a, test[i].b, attr_cmp(test[i].a,
							      test[i].b, 0),
			       test[i].result);
			rv = 1;
		}
	}
	exit(rv);
}

#endif

int __attr_find(struct attrset ***setpp, char *key, int *offsetp, int min)
{
	struct attrset **setp = *setpp;
	struct attrset *set = *setp;
	int i;

	if (!set)
		return -1;
	*offsetp = 0;
	while (set->next &&
	       attr_cmp(key, set->next->attrs, min) >= 0) {
		setp = &set->next;
		set = *setp;
	}
	*setpp = setp;

	for (i = 0; i < set->len; ) {
		int cmp = attr_cmp(key, set->attrs + i, min);
		if (cmp <= 0) {
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

char *attr_find(struct attrset *set, char *key)
{
	int offset = 0;
	struct attrset **setp = &set;
	int cmp = __attr_find(&setp, key, &offset, 0);
	if (cmp != 0)
		return NULL;
	set = *setp;
	offset += strlen(set->attrs + offset) + 1;
	return set->attrs + offset;
}

int attr_del(struct attrset **setp, char *key)
{
	int offset = 0;
	int cmp;
	int len;
	struct attrset *set;

	cmp = __attr_find(&setp, key, &offset, 0);

	if (cmp)
		/* Not found */
		return 0;
	set = *setp;
	len = strlen(set->attrs + offset) + 1;
	len += strlen(set->attrs + offset + len) + 1;
	memmove(set->attrs + offset,
		set->attrs + offset + len,
		set->len - offset);
	set->len -= len;
	if (set->len == 0) {
		*setp = set->next;
		free(set);
	}
	return 1;
}

int attr_set(struct attrset **setp, char *key, char *val, int min)
{
	int offset = 0;
	int cmp;
	struct attrset *set;
	unsigned int len;

	cmp = __attr_find(&setp, key, &offset, min);

	if (cmp == 0) {
		/* Remove old value */
		set = *setp;
		len = strlen(set->attrs + offset) + 1;
		len += strlen(set->attrs + offset + len) + 1;
		memmove(set->attrs + offset,
			set->attrs + offset + len,
			set->len - offset);
		set->len -= len;
	}
	if (!val)
		return cmp;
	set = *setp;
	len = strlen(key) + 1 + strlen(val) + 1;
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
			struct attrset *new = newattr(NULL, set->len - offset + len);

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
	memmove(set->attrs + offset + len, set->attrs + offset, set->len - offset);
	strcpy(set->attrs + offset, key);
	strcpy(set->attrs + offset + strlen(key) + 1, val);
	set->len += len;
	return cmp;
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
			attr_set(&set, a->key, a->val, 0); continue;
		case Remove:
			if (attr_del(&set, a->key) == 0) {
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
int attr_find_int(struct attrset *set, char *key)
{
	char *val = attr_find(set, key);
	unsigned long rv;
	char *end;

	if (!val)
		return -1;
	rv = strtoul(val, &end, 10);
	if (end == val || *end)
		return -1;
	return rv;
}

int attr_set_int(struct attrset **setp, char *key, int val)
{
	char sval[22];

	sprintf(sval, "%d", val);
	return attr_set(setp, key, sval, 0);
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

void attr_free(struct attrset **setp)
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

void attr_trim(struct attrset **setp, int nkey)
{
	char key[22];
	int offset;
	struct attrset *set;

	sprintf(key, "%d", nkey);
	__attr_find(&setp, key, &offset, 0);
	set = *setp;
	if (!set)
		return;
	set->len = offset;
	if (offset)
		setp = &set->next;
	attr_free(setp);
}

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

			if (n <= nkey && *v == '\0')
				v = NULL;
			attr_set(&newset, k, v, nkey);
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
			while (*e == ' ')
				e++;
			if (prefix >= 0) {
				snprintf(kbuf, 512, "%d %s", prefix, e);
				e = kbuf;
			}
			if (*v == '\0')
				v = NULL;
			attr_set(&newset, e, v, 0);
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
		attr_set(&set, keys[i], keys[i], 0);

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
Iterator for set.
*/
