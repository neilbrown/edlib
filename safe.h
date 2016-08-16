#ifndef __CHECK_SAFE__
#define __CHECK_SAFE__ 1
#ifdef __CHECKER__
#define safe __attribute__((safe))
#define safe_cast (void * safe)
extern void *safe calloc(int, int);
extern void *safe malloc(int);
extern void *safe realloc(void *, int);
extern char *safe strdup(char *safe);
extern char *safe strndup(char *safe, int);
extern const unsigned short int **safe __ctype_b_loc(void);
extern int *safe __errno_location(void);
#else
#define safe
#define safe_cast
#endif

#endif /* __CHECK_SAFE__ */
