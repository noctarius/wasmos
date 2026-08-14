/* ctype.h - Minimal character-classification and case-conversion declarations. */
#ifndef WASMOS_LIBC_CTYPE_H
#define WASMOS_LIBC_CTYPE_H

#ifdef __cplusplus
extern "C" {
#endif

/* ASCII-only, locale-free classification. The arguments are plain ints and any
 * value outside the classified ranges — including EOF and bytes above 0x7F — is
 * simply "false" for the is* predicates and returned unchanged by tolower and
 * toupper, so no value is invalid to pass. The predicates return 1/0.
 * isspace() accepts space, \t, \n, \r, \f and \v. */
int tolower(int ch);
int toupper(int ch);
int isspace(int ch);
int isdigit(int ch);
int isalpha(int ch);
int isalnum(int ch);
int isxdigit(int ch);

#ifdef __cplusplus
}
#endif

#endif
