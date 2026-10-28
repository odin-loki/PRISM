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
