// std::basic_spanbuf model (src/prism/pir/models/cxx/spanstream). The pir
// stage puts the model headers first on the include path, so <spanstream>
// below is the model. spanstream, ispanstream, and ospanstream are not part
// of the model: those names fail to compile and the pir stage falls back.
#include <spanstream>

// One character into a 4-byte caller buffer, then that byte read back.
int spanbuf_putc_true(int) {
    char buf[4] = {};
    std::basic_spanbuf<char> b(buf, 4);
    b.sputc('A');
    return buf[0];
}

// sputc when the size is already 0: the offset is the size, so this is a check
// and the buffer is not written.
int spanbuf_size0_false(int) {
    char buf[4] = {};
    std::basic_spanbuf<char> b(buf, 0);
    b.sputc('A');
    return buf[0];
}

// A null pointer with a non-zero size is a check at construction. The size
// stays 1: this is not an empty buffer.
int spanbuf_null_false(int) {
    char* p = nullptr;
    std::basic_spanbuf<char> b(p, 1);
    return static_cast<int>(b.size());
}

// One character read from a 4-byte caller buffer. The result is the unsigned
// value of that byte and the offset advances.
int spanbuf_getc_true(int) {
    char buf[4] = {'A', 'B', 'C', 'D'};
    std::basic_spanbuf<char> b(buf, 4);
    return b.sgetc();
}

// sgetc when the size is already 0: the offset is the size, so this is a
// MEM-OOB-READ check and the pointer is not read. -1 is only the end signal
// after that check.
int spanbuf_getc_size0_false(int) {
    char buf[4] = {'A', 'B', 'C', 'D'};
    std::basic_spanbuf<char> b(buf, 0);
    return b.sgetc();
}

// A null pointer with size 0 is empty. sgetc does not read it. The offset is
// already the size, so this is the same MEM-OOB-READ check.
int spanbuf_getc_null0_false(int) {
    char* p = nullptr;
    std::basic_spanbuf<char> b(p, 0);
    return b.sgetc();
}
