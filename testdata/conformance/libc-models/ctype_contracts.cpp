/* <ctype.h> models (src/prism/pir/models/libc/ctype.cpp) against C17 7.4,
 * C locale, ASCII. See harness.h. Each function is one contract; `_false`
 * ones must fail.
 * This is not a proof of a locale other than the C locale, and it is not PROVED and not CLEAN. */
#include "harness.h"
#include "../../../src/prism/pir/models/libc/ctype.cpp"

/* 7.4.1.5: nonzero only for '0'..'9'. A letter is not a digit. EOF yields 0. */
int isdigit_true(int c) {
    assert(isdigit('0') != 0);
    assert(isdigit('9') != 0);
    assert(isdigit('a') == 0);
    assert(isdigit('A') == 0);
    assert(isdigit(EOF) == 0);
    if (c >= '0' && c <= '9') assert(isdigit(c) != 0);
    else assert(isdigit(c) == 0);
    return 0;
}

/* Wrong contract: a letter is a digit. */
int isdigit_letter_false(void) {
    assert(isdigit('a') != 0);
    return 0;
}

/* 7.4.1.10: the six C locale standard white-space characters, and no others. */
int isspace_true(int c) {
    assert(isspace(' ') != 0);
    assert(isspace('\t') != 0);
    assert(isspace('\n') != 0);
    assert(isspace('\v') != 0);
    assert(isspace('\f') != 0);
    assert(isspace('\r') != 0);
    assert(isspace('a') == 0);
    assert(isspace('0') == 0);
    assert(isspace(EOF) == 0);
    if (c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r')
        assert(isspace(c) != 0);
    else
        assert(isspace(c) == 0);
    return 0;
}

/* Wrong contract: a letter is white space. */
int isspace_letter_false(void) {
    assert(isspace('a') != 0);
    return 0;
}

/* 7.4.1.2: nonzero only for 'A'..'Z' and 'a'..'z'. */
int isalpha_true(int c) {
    assert(isalpha('A') != 0);
    assert(isalpha('Z') != 0);
    assert(isalpha('a') != 0);
    assert(isalpha('z') != 0);
    assert(isalpha('0') == 0);
    assert(isalpha(EOF) == 0);
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) assert(isalpha(c) != 0);
    else assert(isalpha(c) == 0);
    return 0;
}

/* Wrong contract: a digit is a letter. */
int isalpha_digit_false(void) {
    assert(isalpha('0') != 0);
    return 0;
}

/* 7.4.1.1: a digit or a letter. '_' is neither. */
int isalnum_true(int c) {
    assert(isalnum('0') != 0);
    assert(isalnum('A') != 0);
    assert(isalnum('a') != 0);
    assert(isalnum('_') == 0);
    assert(isalnum(' ') == 0);
    assert(isalnum(EOF) == 0);
    if (isdigit(c) != 0 || isalpha(c) != 0) assert(isalnum(c) != 0);
    else assert(isalnum(c) == 0);
    return 0;
}

/* Wrong contract: a space is alphanumeric. */
int isalnum_space_false(void) {
    assert(isalnum(' ') != 0);
    return 0;
}

/* 7.4.1.7: nonzero only for 'a'..'z'. */
int islower_true(int c) {
    assert(islower('a') != 0);
    assert(islower('z') != 0);
    assert(islower('A') == 0);
    assert(islower('0') == 0);
    assert(islower(EOF) == 0);
    if (c >= 'a' && c <= 'z') assert(islower(c) != 0);
    else assert(islower(c) == 0);
    return 0;
}

/* Wrong contract: an uppercase letter is lowercase. */
int islower_upper_false(void) {
    assert(islower('A') != 0);
    return 0;
}

/* 7.4.1.11: nonzero only for 'A'..'Z'. */
int isupper_true(int c) {
    assert(isupper('A') != 0);
    assert(isupper('Z') != 0);
    assert(isupper('a') == 0);
    assert(isupper('0') == 0);
    assert(isupper(EOF) == 0);
    if (c >= 'A' && c <= 'Z') assert(isupper(c) != 0);
    else assert(isupper(c) == 0);
    return 0;
}

/* Wrong contract: a lowercase letter is uppercase. */
int isupper_lower_false(void) {
    assert(isupper('a') != 0);
    return 0;
}

/* 7.4.2.1: 'A' maps to 'a'. A non-letter other than EOF is unchanged. */
int tolower_true(int c) {
    assert(tolower('A') == 'a');
    assert(tolower('Z') == 'z');
    assert(tolower('a') == 'a');
    assert(tolower('0') == '0');
    assert(tolower(' ') == ' ');
    assert(tolower(EOF) == EOF);
    if (c == EOF) assert(tolower(c) == EOF);
    else if (c >= 'A' && c <= 'Z') assert(tolower(c) == c - 'A' + 'a');
    else assert(tolower(c) == c);
    return 0;
}

/* Wrong contract: a non-letter becomes a different letter. */
int tolower_digit_false(void) {
    assert(tolower('0') == 'a');
    return 0;
}

/* 7.4.2.2: symmetric to tolower. 'a' maps to 'A'. */
int toupper_true(int c) {
    assert(toupper('a') == 'A');
    assert(toupper('z') == 'Z');
    assert(toupper('A') == 'A');
    assert(toupper('0') == '0');
    assert(toupper(' ') == ' ');
    assert(toupper(EOF) == EOF);
    assert(toupper(tolower('G')) == 'G');
    assert(tolower(toupper('g')) == 'g');
    if (c == EOF) assert(toupper(c) == EOF);
    else if (c >= 'a' && c <= 'z') assert(toupper(c) == c - 'a' + 'A');
    else assert(toupper(c) == c);
    return 0;
}

/* Wrong contract: a non-letter becomes a different letter. */
int toupper_digit_false(void) {
    assert(toupper('0') == 'A');
    return 0;
}

/* 7.4.1.3: nonzero only for space and horizontal tab. EOF yields 0. */
int isblank_true(int c) {
    assert(isblank(' ') != 0);
    assert(isblank('\t') != 0);
    assert(isblank('\n') == 0);
    assert(isblank('\v') == 0);
    assert(isblank('a') == 0);
    assert(isblank(EOF) == 0);
    if (c == ' ' || c == '\t') assert(isblank(c) != 0);
    else assert(isblank(c) == 0);
    return 0;
}

/* Wrong contract: a newline is a blank. */
int isblank_newline_false(void) {
    assert(isblank('\n') != 0);
    return 0;
}

/* 7.4.1.12: nonzero only for '0'..'9', 'A'..'F', and 'a'..'f'. */
int isxdigit_true(int c) {
    assert(isxdigit('0') != 0);
    assert(isxdigit('9') != 0);
    assert(isxdigit('A') != 0);
    assert(isxdigit('F') != 0);
    assert(isxdigit('a') != 0);
    assert(isxdigit('f') != 0);
    assert(isxdigit('G') == 0);
    assert(isxdigit('g') == 0);
    assert(isxdigit(EOF) == 0);
    if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f'))
        assert(isxdigit(c) != 0);
    else
        assert(isxdigit(c) == 0);
    return 0;
}

/* Wrong contract: 'G' is a hexadecimal digit. */
int isxdigit_letter_false(void) {
    assert(isxdigit('G') != 0);
    return 0;
}

/* 7.4.1.8: nonzero only for 0x20 through 0x7E. */
int isprint_true(int c) {
    assert(isprint(0x20) != 0);
    assert(isprint(0x7E) != 0);
    assert(isprint(0x1F) == 0);
    assert(isprint(0x7F) == 0);
    assert(isprint(EOF) == 0);
    if (c >= 0x20 && c <= 0x7E) assert(isprint(c) != 0);
    else assert(isprint(c) == 0);
    return 0;
}

/* Wrong contract: DEL is printable. */
int isprint_del_false(void) {
    assert(isprint(0x7F) != 0);
    return 0;
}

/* 7.4.1.6: nonzero only for 0x21 through 0x7E. */
int isgraph_true(int c) {
    assert(isgraph(0x21) != 0);
    assert(isgraph(0x7E) != 0);
    assert(isgraph(0x20) == 0);
    assert(isgraph(0x7F) == 0);
    assert(isgraph(EOF) == 0);
    if (c >= 0x21 && c <= 0x7E) assert(isgraph(c) != 0);
    else assert(isgraph(c) == 0);
    return 0;
}

/* Wrong contract: space is graphic. */
int isgraph_space_false(void) {
    assert(isgraph(0x20) != 0);
    return 0;
}

/* 7.4.1.9: a graphic character that is not a digit and not a letter. */
int ispunct_true(int c) {
    assert(ispunct('!') != 0);
    assert(ispunct(' ') == 0);
    assert(ispunct('0') == 0);
    assert(ispunct('A') == 0);
    assert(ispunct('a') == 0);
    assert(ispunct(EOF) == 0);
    if (isgraph(c) != 0 && isdigit(c) == 0 && isalpha(c) == 0) assert(ispunct(c) != 0);
    else assert(ispunct(c) == 0);
    return 0;
}

/* Wrong contract: a letter is punctuation. */
int ispunct_letter_false(void) {
    assert(ispunct('A') != 0);
    return 0;
}

/* 7.4.1.4: nonzero only for 0x00 through 0x1F and 0x7F. */
int iscntrl_true(int c) {
    assert(iscntrl(0x00) != 0);
    assert(iscntrl(0x1F) != 0);
    assert(iscntrl(0x7F) != 0);
    assert(iscntrl(0x20) == 0);
    assert(iscntrl('A') == 0);
    assert(iscntrl(EOF) == 0);
    if ((c >= 0x00 && c <= 0x1F) || c == 0x7F) assert(iscntrl(c) != 0);
    else assert(iscntrl(c) == 0);
    return 0;
}

/* Wrong contract: a space is a control character. */
int iscntrl_space_false(void) {
    assert(iscntrl(0x20) != 0);
    return 0;
}
