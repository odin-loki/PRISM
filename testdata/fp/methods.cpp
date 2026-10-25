// Correct C++: out-of-line and in-class methods, constructor, destructor.
#include <cstdlib>
#include <cstring>

class Buffer {
public:
    explicit Buffer(std::size_t n) : size_(n), data_(static_cast<char*>(std::malloc(n))) {}
    ~Buffer() { std::free(data_); }
    Buffer(const Buffer&) = delete;
    Buffer& operator=(const Buffer&) = delete;
    bool ok() const { return data_ != nullptr; }
    int fill(char c);
    std::size_t size() const { return size_; }

private:
    std::size_t size_;
    char* data_;
};

int Buffer::fill(char c) {
    if (!data_) return -1;
    std::memset(data_, c, size_);
    return 0;
}

int use_buffer() {
    Buffer b(16);
    if (!b.ok()) return -1;
    return b.fill('x');
}
