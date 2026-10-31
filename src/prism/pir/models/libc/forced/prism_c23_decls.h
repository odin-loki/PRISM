/* Force-included into every C translation unit PIR lowers (stage.cpp
 * base_flags, -include). Declares C23 library functions the frontend's libc
 * may not declare yet, so C23 code calling them compiles; their operational
 * models are in ../string.cpp. Each declaration is the C23 prototype, so a
 * libc that does declare it gives a compatible redeclaration (C23 6.7.1). */
#ifndef PRISM_PIR_FORCED_C23_DECLS_H
#define PRISM_PIR_FORCED_C23_DECLS_H
#ifndef __cplusplus
/* C23 7.26.6.2 */
void *memset_explicit(void *, int, __SIZE_TYPE__);
#endif
#endif
