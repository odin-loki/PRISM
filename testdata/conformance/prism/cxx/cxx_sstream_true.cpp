#include <sstream>

int cxx_sstream_true(int x) {
    std::ostringstream os;
    os << x;
    return os.str().size() > 0 ? 0 : 1;
}
