/* Minimal <assert.h> for PIR on Windows (matches glibc's assert -> __assert_fail, not abort/oom). */
#ifndef PRISM_PIR_SHIM_ASSERT_H
#define PRISM_PIR_SHIM_ASSERT_H
void __assert_fail(const char* assertion, const char* file, unsigned int line, const char* function);
#define assert(expr) \
    ((expr) ? (void)0 : __assert_fail("assertion failed", __FILE__, __LINE__, __func__))
#endif
