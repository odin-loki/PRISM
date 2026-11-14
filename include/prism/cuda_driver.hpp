#pragma once

// Shared CUDA driver loader (docs/GPU_PLAN.md). The CUDA driver (libcuda.so.1 /
// nvcuda.dll) is opened at run time through FORGE's dl wrapper and used through
// the driver API only: no cudart, no nvcc, no CUDA SDK at build or link time.
// Kernels are PTX images built by clang's nvptx64 target and embedded in PRISM
// (CMakeLists.txt, cmake/embed_ptx.cmake); the driver JIT-compiles them for the
// card that is there.
//
// Every device allocation is charged to FORGE's memory governor pool "cuda:0"
// (the same accounting FORGE inference uses on that card), so PRISM and the
// models never oversubscribe it between them.
//
// No GPU, no driver, no PTX: available() is false and why() says which. Callers
// must report that (Law 7); a missing GPU is never a clean result (Law 1).

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

namespace forge::mem {
class lease;
}

namespace prism::cuda {

using Kernel = void*;  // CUfunction
using StreamHandle = void*;  // CUstream

class Driver;

// Device memory with a FORGE governor lease; freed (and the lease released) on destruction.
class Buffer {
public:
    Buffer();
    Buffer(Buffer&& o) noexcept;
    Buffer& operator=(Buffer&& o) noexcept;
    Buffer(const Buffer&) = delete;
    Buffer& operator=(const Buffer&) = delete;
    ~Buffer();

    std::uint64_t ptr() const { return ptr_; }  // CUdeviceptr
    template <class T>
    T* as() const {
        return reinterpret_cast<T*>(static_cast<std::uintptr_t>(ptr_));
    }
    std::size_t bytes() const { return bytes_; }
    explicit operator bool() const { return ptr_ != 0; }

    // Synchronous copies (offset in bytes); false with *why on a driver error.
    bool upload(const void* src, std::size_t n, std::size_t offset = 0, std::string* why = nullptr);
    bool download(void* dst, std::size_t n, std::size_t offset = 0, std::string* why = nullptr) const;
    bool zero(std::string* why = nullptr);
    void reset();

private:
    friend class Driver;
    std::uint64_t ptr_ = 0;
    std::size_t bytes_ = 0;
    std::unique_ptr<forge::mem::lease> lease_;
};

// A non-blocking stream: work on it does not wait for (or hold up) other PRISM
// users of the same context, e.g. the PIR VM sweep.
class Stream {
public:
    Stream() = default;
    Stream(Stream&& o) noexcept : s_(o.s_) { o.s_ = nullptr; }
    Stream& operator=(Stream&& o) noexcept;
    Stream(const Stream&) = delete;
    Stream& operator=(const Stream&) = delete;
    ~Stream();
    StreamHandle handle() const { return s_; }

private:
    friend class Driver;
    StreamHandle s_ = nullptr;
};

class Driver {
public:
    // The process-wide driver, initialised on first use and never destroyed
    // (the driver library may already be gone at exit).
    static Driver& get();

    bool available() const { return ok_; }
    const std::string& why() const { return why_; }  // empty when available
    const std::string& device_name() const { return name_; }
    int sm_count() const { return sms_; }
    int compute_capability() const { return cc_; }  // e.g. 86
    std::uint64_t total_memory() const { return total_; }
    // "NVIDIA GeForce RTX 3090, 82 SMs, sm_86, 24.0 GiB, CUDA driver via FORGE dl", or why not.
    std::string describe() const;
    // Free and total device memory now (cuMemGetInfo); false when unavailable.
    bool mem_info(std::size_t* free_bytes, std::size_t* total_bytes);
    // Bytes the FORGE pool "cuda:0" still has (UINT64_MAX when it has no limit).
    static std::uint64_t pool_available();

    // Load a NUL-terminated PTX image once (cached by name for the process).
    // False with *why (including the JIT log) when the driver rejects it.
    bool load_module(const std::string& name, const char* ptx, std::size_t size, std::string* why);
    // A kernel of a loaded module; nullptr with *why.
    Kernel kernel(const std::string& module, const char* entry, std::string* why);

    // cuMemAlloc behind a governor lease tagged `tag`; an empty Buffer with *why
    // when the pool or the device is full.
    Buffer alloc(std::size_t bytes, const char* tag, std::string* why);
    Stream stream(std::string* why);

    // Launch with one by-value parameter struct (the kernel's only argument).
    template <class P>
    bool launch(Kernel k, unsigned grid, unsigned block, const P& params, const Stream& s, std::string* why) {
        void* args[] = {const_cast<P*>(&params)};
        return launch_raw(k, grid, block, args, s.handle(), why);
    }
    bool launch_raw(Kernel k, unsigned grid, unsigned block, void** args, StreamHandle s, std::string* why);
    bool sync(const Stream& s, std::string* why);

    std::string error_string(int r) const;
    // Make the primary context current on the calling thread.
    bool make_current(std::string* why = nullptr);

    struct Api;  // driver entry points (cuda_driver.cpp)

private:
    friend class Buffer;
    friend class Stream;
    Driver();
    void init();
    void free_ptr(std::uint64_t p);

    std::unique_ptr<Api> api_;
    std::mutex mu_;
    bool ok_ = false;
    std::string why_ = "not initialised";
    std::string name_;
    int sms_ = 0, cc_ = 0;
    std::uint64_t total_ = 0;
    void* ctx_ = nullptr;
};

}  // namespace prism::cuda
