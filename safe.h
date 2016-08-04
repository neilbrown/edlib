#ifndef __CHECK_SAFE__
#define __CHECK_SAFE__ 1
#ifdef __CHECKER__
#define safe __attribute__((safe))
#define safe_cast (void * safe)
extern void *calloc(int, int) safe;
extern void *malloc(int) safe;
extern void *realloc(void *, int) safe;
extern char *strdup(char *) safe;
extern char *strndup(char *, int) safe;
extern const unsigned short int **__ctype_b_loc(void) safe;
extern int *__errno_location(void) safe;
#else
#define safe
#define safe_cast
#endif

#endif /* __CHECK_SAFE__ */
