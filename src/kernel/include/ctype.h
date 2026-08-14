/* ctype.h - Kernel-internal character-classification and case-conversion stubs. */
#ifndef WASMOS_CTYPE_H
#define WASMOS_CTYPE_H

/* ASCII only, with no locale: every predicate returns non-zero for a match and 0
 * otherwise, and the two converters return the input unchanged when it is not a letter of
 * the opposite case.  isspace accepts space, tab, newline, carriage return, vertical tab
 * and form feed.  Unlike the C standard versions these take the character value directly
 * and are safe with any int, including a negative one from a sign-extended char. */
int tolower(int ch);
int toupper(int ch);
int isspace(int ch);
int isdigit(int ch);
int isalpha(int ch);
int isalnum(int ch);
int isxdigit(int ch);

#endif
