// Names <initializer_list> so the C++ model-header conformance case sees
// this include. The scored walk is
// testdata/conformance/prism/cxx/cxx_initializer_list_true.cpp.
// There is no false twin: this header does not check a defect of its own.
// A past-the-end read would be the caller's arithmetic, not a check the
// model plants.
#include <initializer_list>

int initializer_list_walk_true(int c) {
    if (c < 0) return 0;
    std::initializer_list<int> xs{1, 2, 3, 4};
    int s = 0;
    auto it = xs.begin();
    for (decltype(xs.size()) i = 0; i < xs.size(); ++i) {
        s += *it;
        ++it;
    }
    return s;
}
