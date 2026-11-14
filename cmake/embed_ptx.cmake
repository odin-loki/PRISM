# cmake -DIN=pir_vm.ptx -DOUT=pir_vm_ptx.inc [-DNAME=kPirVmPtx] [-DSRC=src/cuda/pir_vm.cu] -P embed_ptx.cmake
# Writes the PTX as a NUL-terminated char array (cuModuleLoadDataEx wants a C string):
#   const char ${NAME}[]; const std::size_t ${NAME}Size;
# NAME defaults to kPirVmPtx and SRC to src/cuda/pir_vm.cu (the first caller).
if(NOT DEFINED NAME OR NAME STREQUAL "")
  set(NAME "kPirVmPtx")
endif()
if(NOT DEFINED SRC OR SRC STREQUAL "")
  set(SRC "src/cuda/pir_vm.cu")
endif()
file(READ "${IN}" _hex HEX)
string(LENGTH "${_hex}" _n)
math(EXPR _bytes "${_n} / 2")
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," _arr "${_hex}")
string(REGEX REPLACE "((0x[0-9a-f][0-9a-f],){32})" "\\1\n" _arr "${_arr}")
file(WRITE "${OUT}.tmp"
  "// generated from ${SRC} by cmake/embed_ptx.cmake (do not edit)\n"
  "const char ${NAME}[] = {\n${_arr}0x00};\n"
  "const std::size_t ${NAME}Size = ${_bytes};\n")
file(RENAME "${OUT}.tmp" "${OUT}")
