// std::mutex operational model (src/prism/pir/models/cxx/mutex) against
// C++23 [thread.mutex.class]. The pir stage puts the model headers first on
// the include path, so <mutex> below is the model. The locked flag is in the
// object: there is no heap and no thread. This is not a concurrency proof.
// lock, timed_mutex, recursive_mutex, scoped_lock, lock_guard, and unique_lock
// are not part of the model. Each function is one contract; `_false` ones
// must fail for the class their yml names.
#include <cassert>
#include <mutex>

// try_lock once, then a second try_lock is false. The one acquisition is released.
int mutex_trylock_true(int c) {
    if (c == 0) return 0;
    std::mutex m;
    assert(m.try_lock());
    assert(!m.try_lock());
    m.unlock();
    return 0;
}

// unlock of a held mutex clears the flag, so try_lock succeeds again.
int mutex_relock_true(int c) {
    if (c == 0) return 0;
    std::mutex m;
    assert(m.try_lock());
    m.unlock();
    assert(m.try_lock());
    m.unlock();
    return 0;
}

// A wrong contract: the second try_lock returns true while the mutex is locked.
int mutex_second_trylock_false(int c) {
    if (c == 0) return 0;
    std::mutex m;
    assert(m.try_lock());
    assert(m.try_lock());
    m.unlock();
    return 0;
}

// unlock on a mutex that was never locked (c == 0).
int mutex_unlock_unlocked_false(int c) {
    std::mutex m;
    if (c) {
        assert(m.try_lock());
        m.unlock();
        return 0;
    }
    m.unlock();
    return 0;
}
