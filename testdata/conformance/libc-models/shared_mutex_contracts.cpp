// std::shared_mutex operational model (src/prism/pir/models/cxx/shared_mutex).
// The pir stage puts the model headers first on the include path, so
// <shared_mutex> below is the model. The object stores an exclusive flag and
// a shared count. There is no heap and no thread. lock() and lock_shared()
// are not part of the model.
#include <shared_mutex>

// One shared hold. Exclusive try_lock is false. unlock_shared releases it.
int shared_mutex_shared_true(int c) {
    std::shared_mutex m;
    if (!m.try_lock_shared()) return 1;
    if (m.try_lock()) return 1;
    m.unlock_shared();
    (void)c;
    return 0;
}

// Exclusive hold. A shared try is false. unlock releases it.
int shared_mutex_exclusive_true(int c) {
    std::shared_mutex m;
    if (!m.try_lock()) return 1;
    if (m.try_lock_shared()) return 1;
    m.unlock();
    (void)c;
    return 0;
}

// unlock_shared when the shared count is 0.
int shared_mutex_unlock_shared_false(int c) {
    std::shared_mutex m;
    if (c) return 0;
    m.unlock_shared();
    return 0;
}

// unlock when the exclusive flag is clear.
int shared_mutex_unlock_false(int c) {
    std::shared_mutex m;
    if (c) return 0;
    m.unlock();
    return 0;
}
