/* ctype.h - Minimal character-classification and case-conversion declarations. */
#ifndef WASMOS_LIBC_CTYPE_H
#define WASMOS_LIBC_CTYPE_H

#ifdef __cplusplus
extern "C" {
#endif

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
