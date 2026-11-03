// std::ifstream operational model (src/prism/pir/models/cxx/fstream). The pir
// stage puts the model headers first on the include path, so <fstream> below
// is the model. A default stream is not open. read and get on it are a
// FUNC-CONTRACT check: they do not write a byte and do not return a
// character from a file. open, close, ofstream, and exists are not part of
// the model. A successful open would be a fake file. This file is not a
// proof that a file exists.
#include <fstream>

int ifstream_closed_true(int) {
    std::ifstream in;
    if (in.is_open()) return 1;
    return 0;
}

// read on a stream that is not open does not store into the buffer.
int ifstream_read_closed_false(int) {
    std::ifstream in;
    char buf[4] = {'a', 'b', 'c', 'd'};
    in.read(buf, 4);
    if (buf[0] != 'a' || buf[1] != 'b' || buf[2] != 'c' || buf[3] != 'd') return 1;
    return 0;
}

// get on a stream that is not open does not store a character.
int ifstream_get_closed_false(int) {
    std::ifstream in;
    char c = 'Q';
    in.get(c);
    if (c != 'Q') return 1;
    (void)in.get();
    return 0;
}
