/*
 * Copyright Neil Brown <neil@brown.name> 2015
 * May be distrubuted under terms of GPLv2 - see file:COPYING
 */

struct attrset;
char *attr_find(struct attrset *set, char *key);
int attr_del(struct attrset **setp, char *key);
int attr_set_str(struct attrset **setp, char *key, char *val, int keynum);
char *attr_get_str(struct attrset *setp, char *key, int keynum);
int attr_find_int(struct attrset *set, char *key);
int attr_set_int(struct attrset **setp, char *key, int val);
void attr_trim(struct attrset **setp, int nkey);
struct attrset *attr_copy_tail(struct attrset *set, int nkey);
struct attrset *attr_collect(struct attrset *set, int pos, int prefix);
void attr_free(struct attrset **setp);
