/*
 * Copyright Neil Brown ©2016-2019 <neil@brown.name>
 * May be distributed under terms of GPLv2 - see file:COPYING
 */
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
extern int strcmp(char *a safe, char *b safe);
extern int strncmp(char *a safe, char *b safe, int n);
extern char *safe strcpy(char *a safe, char *b safe);
extern char *safe strncpy(char *a safe, char *b safe, int n);
extern char *safe strcat(char *a safe, char *b safe);
extern char *safe strncat(char *a safe, char *b safe, int n);
extern char *strchr(const char *a safe, char b);
extern char *safe strchrnul(const char *a safe, char b);
extern char *strrchr(const char *a safe, char b);
extern int strlen(const char *a safe);

extern const unsigned short int **safe __ctype_b_loc(void);
extern int *safe __errno_location(void);
#else
#define safe
#define safe_cast
#endif

#endif /* __CHECK_SAFE__ */
