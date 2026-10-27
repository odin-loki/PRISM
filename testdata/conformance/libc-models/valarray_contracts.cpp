// std::valarray model (src/prism/pir/models/cxx/valarray). The pir stage puts
// the model directory first on the include path, so <valarray> below is the
// model. The false function indexes an empty valarray.
#include <valarray>

int valarray_size_true(int c) {
    std::valarray<int> v;
    if (c < 0) return 0;
    return static_cast<int>(v.size());
}

int valarray_read_true(int c) {
    std::valarray<int> v;
    v.resize(1, c);
    return v[0];
}

int valarray_empty_index_false(int c) {
    std::valarray<int> v;
    if (c) {
        v.resize(1, c);
        return v[0];
    }
    return v[0];
}
