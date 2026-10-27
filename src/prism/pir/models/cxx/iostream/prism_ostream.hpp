// Narrow recording buffer for formatted inserts (docs/finish-notes/iostream.md).
//
// This is not <iostream>, not std::basic_ostream, and not a file the pir
// stage can put on the include path in place of a libstdc++ header. Include
// it by path. It does not define _GLIBCXX_IOSTREAM / _GLIBCXX_OSTREAM /
// _GLIBCXX_SSTREAM, so it cannot hide those headers.
//
// What it records: a decimal int, a C string, or a counted character range,
// appended to a buffer with the same storage shape as the basic_string model
// (src/prism/pir/models/cxx/bits/basic_string.tcc): a 15-character local
// buffer, _M_create's growth (a request below twice the old capacity becomes
// twice that capacity), storage from ::operator new / ::operator delete, and
// element loops instead of memcpy. One extra byte holds a trailing NUL, so
// data() is a C string of size() characters. The heap block is capacity()+1
// bytes.
//
// What "failed" means: the same observable outcome as a stream that has
// already failed a write (stdio's EOF / short write once the error is known,
// and an ostream sentry that does not insert). set_bad() enters that state.
// A null string with a non-zero length, or a length that does not fit, enters
// it too. Further inserts record nothing and the characters already stored
// stay. The flag is sticky. This header is included by ordinary tests, so a
// failure is that explicit state, not a nondet branch on every insert.
//
// Not here: floats, pointers, manipulators, width, base, locales, wchar_t,
// streambuf, cin/cout, operator>> / getline, and any real iostream header.
#ifndef PRISM_CXX_OSTREAM_MODEL_HPP
#define PRISM_CXX_OSTREAM_MODEL_HPP

#include <cstddef>
#include <new>

namespace prism::cxx_model {

class ostream_buf {
public:
    static constexpr std::size_t local_capacity = 15;

    // Largest character count grow_to will try to store (not counting the
    // trailing NUL). A request past this fails the stream and leaves the
    // buffer as it was.
    static constexpr std::size_t max_size() noexcept { return static_cast<std::size_t>(-1) / 2; }

    ostream_buf() noexcept : data_(local_), size_(0), capacity_(local_capacity), heap_(false), failed_(false) {
        for (std::size_t i = 0; i < local_capacity + 1; ++i) local_[i] = '\0';
    }

    ~ostream_buf() {
        if (heap_) ::operator delete(data_);
    }

    ostream_buf(const ostream_buf&) = delete;
    ostream_buf& operator=(const ostream_buf&) = delete;

    ostream_buf(ostream_buf&& other) noexcept
        : data_(local_), size_(other.size_), capacity_(local_capacity), heap_(false), failed_(other.failed_) {
        if (other.heap_) {
            data_ = other.data_;
            capacity_ = other.capacity_;
            heap_ = true;
            other.data_ = other.local_;
            other.capacity_ = local_capacity;
            other.heap_ = false;
        } else {
            for (std::size_t i = 0; i < other.size_; ++i) local_[i] = other.local_[i];
            local_[other.size_] = '\0';
        }
        other.size_ = 0;
        other.failed_ = false;
        other.local_[0] = '\0';
    }

    ostream_buf& operator=(ostream_buf&& other) noexcept {
        if (this == &other) return *this;
        if (heap_) ::operator delete(data_);
        failed_ = other.failed_;
        size_ = other.size_;
        if (other.heap_) {
            data_ = other.data_;
            capacity_ = other.capacity_;
            heap_ = true;
            other.data_ = other.local_;
            other.capacity_ = local_capacity;
            other.heap_ = false;
        } else {
            data_ = local_;
            capacity_ = local_capacity;
            heap_ = false;
            for (std::size_t i = 0; i < other.size_; ++i) local_[i] = other.local_[i];
            local_[other.size_] = '\0';
        }
        other.size_ = 0;
        other.failed_ = false;
        other.local_[0] = '\0';
        return *this;
    }

    // Enter the failed state. Later inserts record nothing.
    void set_bad() noexcept { failed_ = true; }

    bool failed() const noexcept { return failed_; }

    // True when the stream has not failed (the same polarity as ostream's
    // contextual conversion).
    explicit operator bool() const noexcept { return !failed_; }

    ostream_buf& operator<<(int value) {
        if (failed_) return *this;
        char buf[64];
        std::size_t n = 0;
        const bool neg = value < 0;
        // value + 1 is representable even for the most negative int; the
        // magnitude is completed in an unsigned type.
        unsigned long long mag = neg ? static_cast<unsigned long long>(-(value + 1)) + 1ull
                                     : static_cast<unsigned long long>(value);
        char rev[64];
        std::size_t rn = 0;
        do {
            rev[rn++] = static_cast<char>('0' + static_cast<int>(mag % 10ull));
            mag /= 10ull;
        } while (mag != 0);
        if (neg) buf[n++] = '-';
        while (rn > 0) buf[n++] = rev[--rn];
        return append(buf, n);
    }

    ostream_buf& operator<<(const char* text) {
        if (failed_) return *this;
        if (text == nullptr) {
            failed_ = true;
            return *this;
        }
        std::size_t n = 0;
        while (text[n] != '\0') ++n;
        return append(text, n);
    }

    // Counted insert. n == 0 records nothing and does not fail, including
    // when text is null. A null text with n > 0 fails the stream.
    ostream_buf& write(const char* text, std::size_t n) {
        if (failed_) return *this;
        if (n == 0) return *this;
        if (text == nullptr) {
            failed_ = true;
            return *this;
        }
        return append(text, n);
    }

    const char* data() const noexcept { return data_; }
    std::size_t size() const noexcept { return size_; }
    std::size_t capacity() const noexcept { return capacity_; }

private:
    char local_[local_capacity + 1];
    char* data_;
    std::size_t size_;
    std::size_t capacity_;
    bool heap_;
    bool failed_;

    // Make room for `need` characters plus the trailing NUL. On success the
    // old characters are unchanged. A length past max_size() fails the stream
    // and leaves the buffer unchanged. ::operator new either provides the
    // block or throws before any of this object's pointers change.
    bool grow_to(std::size_t need) {
        if (need <= capacity_) return true;
        if (need > max_size()) {
            failed_ = true;
            return false;
        }
        // basic_string::_M_create: a request strictly below twice the old
        // capacity grows to twice the old capacity, clamped to max_size().
        std::size_t cap = need;
        if (capacity_ > max_size() / 2) {
            cap = max_size();
        } else if (need < capacity_ * 2) {
            cap = capacity_ * 2;
        }
        char* fresh = static_cast<char*>(::operator new(cap + 1));
        for (std::size_t i = 0; i < size_; ++i) fresh[i] = data_[i];
        fresh[size_] = '\0';
        if (heap_) ::operator delete(data_);
        data_ = fresh;
        capacity_ = cap;
        heap_ = true;
        return true;
    }

    ostream_buf& append(const char* text, std::size_t n) {
        if (n == 0) return *this;
        if (n > max_size() - size_) {
            failed_ = true;
            return *this;
        }
        if (!grow_to(size_ + n)) return *this;
        for (std::size_t i = 0; i < n; ++i) data_[size_ + i] = text[i];
        size_ += n;
        data_[size_] = '\0';
        return *this;
    }
};

}  // namespace prism::cxx_model

#endif
