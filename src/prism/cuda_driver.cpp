// Shared CUDA driver loader: libcuda through FORGE dl, driver API only, device
// memory charged to the FORGE governor pool "cuda:0". See include/prism/cuda_driver.hpp.

#include "prism/cuda_driver.hpp"

#include <forge/core/dl.hpp>
#include <forge/core/memory.hpp>

// FORGE os headers may pull in <windows.h>; its ERROR macro breaks laws::ERROR.
#if defined(_WIN32) && defined(ERROR)
#  undef ERROR
#endif

#include <cstdio>
#include <map>
#include <type_traits>
#include <utility>

namespace prism::cuda {

namespace {
using CUresult = int;
using CUdevice = int;
using CUcontext = void*;
using CUmodule = void*;
using CUstream = void*;
using CUdeviceptr = unsigned long long;

constexpr int kAttrSmCount = 16;     // CU_DEVICE_ATTRIBUTE_MULTIPROCESSOR_COUNT
constexpr int kAttrCcMajor = 75;     // CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MAJOR
constexpr int kAttrCcMinor = 76;     // CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MINOR
constexpr int kJitErrorLog = 5;      // CU_JIT_ERROR_LOG_BUFFER
constexpr int kJitErrorLogSize = 6;  // CU_JIT_ERROR_LOG_BUFFER_SIZE_BYTES
constexpr unsigned kStreamNonBlocking = 1;  // CU_STREAM_NON_BLOCKING
}  // namespace

struct Driver::Api {
    void* lib = nullptr;
    CUresult (*cuInit)(unsigned) = nullptr;
    CUresult (*cuDeviceGetCount)(int*) = nullptr;
    CUresult (*cuDeviceGet)(CUdevice*, int) = nullptr;
    CUresult (*cuDeviceGetName)(char*, int, CUdevice) = nullptr;
    CUresult (*cuDeviceGetAttribute)(int*, int, CUdevice) = nullptr;
    CUresult (*cuDeviceTotalMem)(std::size_t*, CUdevice) = nullptr;
    CUresult (*cuDevicePrimaryCtxRetain)(CUcontext*, CUdevice) = nullptr;
    CUresult (*cuCtxSetCurrent)(CUcontext) = nullptr;
    CUresult (*cuModuleLoadDataEx)(CUmodule*, const void*, unsigned, int*, void**) = nullptr;
    CUresult (*cuModuleGetFunction)(Kernel*, CUmodule, const char*) = nullptr;
    CUresult (*cuMemAlloc)(CUdeviceptr*, std::size_t) = nullptr;
    CUresult (*cuMemFree)(CUdeviceptr) = nullptr;
    CUresult (*cuMemGetInfo)(std::size_t*, std::size_t*) = nullptr;
    CUresult (*cuMemcpyHtoD)(CUdeviceptr, const void*, std::size_t) = nullptr;
    CUresult (*cuMemcpyDtoH)(void*, CUdeviceptr, std::size_t) = nullptr;
    CUresult (*cuMemsetD8)(CUdeviceptr, unsigned char, std::size_t) = nullptr;
    CUresult (*cuLaunchKernel)(Kernel, unsigned, unsigned, unsigned, unsigned, unsigned, unsigned, unsigned, CUstream,
                               void**, void**) = nullptr;
    CUresult (*cuStreamCreate)(CUstream*, unsigned) = nullptr;
    CUresult (*cuStreamDestroy)(CUstream) = nullptr;
    CUresult (*cuStreamSynchronize)(CUstream) = nullptr;
    CUresult (*cuGetErrorString)(CUresult, const char**) = nullptr;
    std::map<std::string, CUmodule> modules;
};

namespace {
thread_local void* t_current_ctx = nullptr;

void set_why(std::string* why, std::string m) {
    if (why) *why = std::move(m);
}
}  // namespace

Driver::Driver() : api_(std::make_unique<Api>()) {}

Driver& Driver::get() {
    static Driver* d = [] {
        auto* x = new Driver;  // never destroyed: the driver may be gone at exit
        x->init();
        return x;
    }();
    return *d;
}

std::string Driver::error_string(int r) const {
    const char* s = nullptr;
    if (api_->cuGetErrorString) api_->cuGetErrorString(r, &s);
    return s ? std::string(s) : "CUDA error " + std::to_string(r);
}

void Driver::init() {
    Api& a = *api_;
    a.lib = dlopen(FORGE_DLNAME("libcuda.so.1", "nvcuda.dll"), RTLD_NOW | RTLD_LOCAL);
    if (!a.lib) {
        why_ = "CUDA driver not found (" FORGE_DLNAME("libcuda.so.1", "nvcuda.dll") ")";
        return;
    }
    auto sym = [&](auto& f, const char* n) {
        f = reinterpret_cast<std::remove_reference_t<decltype(f)>>(dlsym(a.lib, n));
        return f != nullptr;
    };
    if (!sym(a.cuInit, "cuInit") || !sym(a.cuDeviceGetCount, "cuDeviceGetCount") ||
        !sym(a.cuDeviceGet, "cuDeviceGet") || !sym(a.cuDeviceGetName, "cuDeviceGetName") ||
        !sym(a.cuDeviceGetAttribute, "cuDeviceGetAttribute") || !sym(a.cuDeviceTotalMem, "cuDeviceTotalMem_v2") ||
        !sym(a.cuDevicePrimaryCtxRetain, "cuDevicePrimaryCtxRetain") || !sym(a.cuCtxSetCurrent, "cuCtxSetCurrent") ||
        !sym(a.cuModuleLoadDataEx, "cuModuleLoadDataEx") || !sym(a.cuModuleGetFunction, "cuModuleGetFunction") ||
        !sym(a.cuMemAlloc, "cuMemAlloc_v2") || !sym(a.cuMemFree, "cuMemFree_v2") ||
        !sym(a.cuMemGetInfo, "cuMemGetInfo_v2") || !sym(a.cuMemcpyHtoD, "cuMemcpyHtoD_v2") ||
        !sym(a.cuMemcpyDtoH, "cuMemcpyDtoH_v2") || !sym(a.cuMemsetD8, "cuMemsetD8_v2") ||
        !sym(a.cuLaunchKernel, "cuLaunchKernel") || !sym(a.cuStreamCreate, "cuStreamCreate") ||
        !sym(a.cuStreamDestroy, "cuStreamDestroy_v2") || !sym(a.cuStreamSynchronize, "cuStreamSynchronize")) {
        why_ = "CUDA driver lacks a required entry point";
        return;
    }
    sym(a.cuGetErrorString, "cuGetErrorString");
    CUresult r = a.cuInit(0);
    if (r != 0) {
        why_ = "cuInit: " + error_string(r);
        return;
    }
    int n = 0;
    if (a.cuDeviceGetCount(&n) != 0 || n <= 0) {
        why_ = "no CUDA device";
        return;
    }
    CUdevice dev = 0;
    a.cuDeviceGet(&dev, 0);
    char nm[256] = {};
    a.cuDeviceGetName(nm, sizeof nm - 1, dev);
    name_ = nm;
    int maj = 0, min = 0;
    a.cuDeviceGetAttribute(&sms_, kAttrSmCount, dev);
    a.cuDeviceGetAttribute(&maj, kAttrCcMajor, dev);
    a.cuDeviceGetAttribute(&min, kAttrCcMinor, dev);
    cc_ = maj * 10 + min;
    std::size_t tot = 0;
    a.cuDeviceTotalMem(&tot, dev);
    total_ = tot;
    if ((r = a.cuDevicePrimaryCtxRetain(&ctx_, dev)) != 0) {
        why_ = "context: " + error_string(r);
        return;
    }
    a.cuCtxSetCurrent(ctx_);
    t_current_ctx = ctx_;
    ok_ = true;
    why_.clear();
}

std::string Driver::describe() const {
    if (!ok_) return "no GPU: " + why_;
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.1f GiB", double(total_) / double(1ull << 30));
    return name_ + ", " + std::to_string(sms_) + " SMs, sm_" + std::to_string(cc_) + ", " + buf +
           ", CUDA driver via FORGE dl";
}

bool Driver::make_current(std::string* why) {
    if (!ok_) {
        set_why(why, why_);
        return false;
    }
    if (t_current_ctx == ctx_) return true;
    CUresult r = api_->cuCtxSetCurrent(ctx_);
    if (r != 0) {
        set_why(why, "cuCtxSetCurrent: " + error_string(r));
        return false;
    }
    t_current_ctx = ctx_;
    return true;
}

bool Driver::mem_info(std::size_t* free_bytes, std::size_t* total_bytes) {
    std::size_t f = 0, t = 0;
    if (!make_current() || api_->cuMemGetInfo(&f, &t) != 0) return false;
    if (free_bytes) *free_bytes = f;
    if (total_bytes) *total_bytes = t;
    return true;
}

std::uint64_t Driver::pool_available() { return forge::mem::governor::instance().pool("cuda:0").available(); }

bool Driver::load_module(const std::string& name, const char* ptx, std::size_t size, std::string* why) {
    if (!make_current(why)) return false;
    if (!ptx || size == 0) {
        set_why(why, "PRISM was built without the " + name +
                         " PTX kernel (clang with the nvptx64 target not found at configure)");
        return false;
    }
    std::lock_guard g(mu_);
    if (api_->modules.count(name)) return true;
    char log[4096] = {};
    int opts[2] = {kJitErrorLog, kJitErrorLogSize};
    void* vals[2] = {log, reinterpret_cast<void*>(std::uintptr_t(sizeof log - 1))};
    CUmodule m = nullptr;
    CUresult r = api_->cuModuleLoadDataEx(&m, ptx, 2, opts, vals);
    if (r != 0) {
        std::string msg = "PTX module " + name + " did not load (" + error_string(r) + ")";
        if (log[0]) msg += ": " + std::string(log);
        set_why(why, msg);
        return false;
    }
    api_->modules[name] = m;
    return true;
}

Kernel Driver::kernel(const std::string& module, const char* entry, std::string* why) {
    if (!make_current(why)) return nullptr;
    CUmodule m = nullptr;
    {
        std::lock_guard g(mu_);
        auto it = api_->modules.find(module);
        if (it == api_->modules.end()) {
            set_why(why, "PTX module " + module + " not loaded");
            return nullptr;
        }
        m = it->second;
    }
    Kernel k = nullptr;
    CUresult r = api_->cuModuleGetFunction(&k, m, entry);
    if (r != 0) {
        set_why(why, std::string("kernel ") + entry + ": " + error_string(r));
        return nullptr;
    }
    return k;
}

Buffer Driver::alloc(std::size_t bytes, const char* tag, std::string* why) {
    Buffer b;
    if (!make_current(why)) return b;
    bytes = bytes ? bytes : 8;
    auto l = forge::mem::governor::instance().pool("cuda:0").try_reserve(bytes, tag ? tag : "prism");
    if (!l) {
        set_why(why, "FORGE device memory pool cuda:0 is full (" + std::to_string(bytes) + " bytes for " +
                         (tag ? tag : "prism") + ")");
        return b;
    }
    CUdeviceptr p = 0;
    CUresult r = api_->cuMemAlloc(&p, bytes);
    if (r != 0) {
        set_why(why, "cuMemAlloc(" + std::to_string(bytes) + "): " + error_string(r));
        return b;
    }
    b.ptr_ = p;
    b.bytes_ = bytes;
    b.lease_ = std::make_unique<forge::mem::lease>(std::move(l));
    return b;
}

void Driver::free_ptr(std::uint64_t p) {
    if (p && make_current()) api_->cuMemFree(p);
}

Stream Driver::stream(std::string* why) {
    Stream s;
    if (!make_current(why)) return s;
    CUresult r = api_->cuStreamCreate(&s.s_, kStreamNonBlocking);
    if (r != 0) {
        s.s_ = nullptr;
        set_why(why, "cuStreamCreate: " + error_string(r));
    }
    return s;
}

bool Driver::launch_raw(Kernel k, unsigned grid, unsigned block, void** args, StreamHandle s, std::string* why) {
    if (!make_current(why)) return false;
    CUresult r = api_->cuLaunchKernel(k, grid ? grid : 1, 1, 1, block ? block : 1, 1, 1, 0, s, args, nullptr);
    if (r != 0) {
        set_why(why, "cuLaunchKernel: " + error_string(r));
        return false;
    }
    return true;
}

bool Driver::sync(const Stream& s, std::string* why) {
    if (!make_current(why)) return false;
    CUresult r = api_->cuStreamSynchronize(s.handle());
    if (r != 0) {
        set_why(why, "kernel: " + error_string(r));
        return false;
    }
    return true;
}

// ---------------------------------------------------------------- Buffer

Buffer::Buffer() = default;
Buffer::Buffer(Buffer&& o) noexcept
    : ptr_(std::exchange(o.ptr_, 0)), bytes_(std::exchange(o.bytes_, 0)), lease_(std::move(o.lease_)) {}
Buffer& Buffer::operator=(Buffer&& o) noexcept {
    if (this != &o) {
        reset();
        ptr_ = std::exchange(o.ptr_, 0);
        bytes_ = std::exchange(o.bytes_, 0);
        lease_ = std::move(o.lease_);
    }
    return *this;
}
Buffer::~Buffer() { reset(); }

void Buffer::reset() {
    if (ptr_) Driver::get().free_ptr(ptr_);
    ptr_ = 0;
    bytes_ = 0;
    lease_.reset();  // the lease goes back to the pool after the memory is freed
}

bool Buffer::upload(const void* src, std::size_t n, std::size_t offset, std::string* why) {
    auto& d = Driver::get();
    if (n == 0) return true;
    if (!ptr_ || offset + n > bytes_) {
        set_why(why, "upload out of range");
        return false;
    }
    if (!d.make_current(why)) return false;
    CUresult r = d.api_->cuMemcpyHtoD(ptr_ + offset, src, n);
    if (r != 0) {
        set_why(why, "copy to device: " + d.error_string(r));
        return false;
    }
    return true;
}

bool Buffer::download(void* dst, std::size_t n, std::size_t offset, std::string* why) const {
    auto& d = Driver::get();
    if (n == 0) return true;
    if (!ptr_ || offset + n > bytes_) {
        set_why(why, "download out of range");
        return false;
    }
    if (!d.make_current(why)) return false;
    CUresult r = d.api_->cuMemcpyDtoH(dst, ptr_ + offset, n);
    if (r != 0) {
        set_why(why, "copy from device: " + d.error_string(r));
        return false;
    }
    return true;
}

bool Buffer::zero(std::string* why) {
    auto& d = Driver::get();
    if (!ptr_) return true;
    if (!d.make_current(why)) return false;
    CUresult r = d.api_->cuMemsetD8(ptr_, 0, bytes_);
    if (r != 0) {
        set_why(why, "cuMemsetD8: " + d.error_string(r));
        return false;
    }
    return true;
}

// ---------------------------------------------------------------- Stream

Stream& Stream::operator=(Stream&& o) noexcept {
    if (this != &o) {
        this->~Stream();
        s_ = std::exchange(o.s_, nullptr);
    }
    return *this;
}

Stream::~Stream() {
    if (s_) {
        auto& d = Driver::get();
        if (d.make_current()) d.api_->cuStreamDestroy(s_);
        s_ = nullptr;
    }
}

}  // namespace prism::cuda
