#include <sstream>

int cxx_sstream_false(int x) {
    std::ostringstream os;
    os << x;
    return os.str().empty() ? 0 : 1;
}
