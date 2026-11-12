/*
 * Enough of POSIX threads for imagestash on MSVC, which has no pthread.h.
 * pthread_t is a pointer so pthread_join can take it by value. Mutex and
 * condition members are real C++ objects; init/destroy are no-ops around
 * those constructors.
 */
#ifndef NANOGUI_PTHREAD_WIN_H
#define NANOGUI_PTHREAD_WIN_H

#include <condition_variable>
#include <mutex>
#include <thread>

struct pthread_mutex_t {
    std::mutex mu;
};

struct pthread_cond_t {
    std::condition_variable cv;
};

struct nanogui_pthread {
    std::thread thread;
};

typedef nanogui_pthread *pthread_t;

inline int pthread_mutex_init(pthread_mutex_t *, const void *) { return 0; }
inline int pthread_mutex_destroy(pthread_mutex_t *) { return 0; }
inline int pthread_mutex_lock(pthread_mutex_t *m) { m->mu.lock(); return 0; }
inline int pthread_mutex_unlock(pthread_mutex_t *m) { m->mu.unlock(); return 0; }

inline int pthread_cond_init(pthread_cond_t *, const void *) { return 0; }
inline int pthread_cond_destroy(pthread_cond_t *) { return 0; }
inline int pthread_cond_broadcast(pthread_cond_t *c) { c->cv.notify_all(); return 0; }
inline int pthread_cond_signal(pthread_cond_t *c) { c->cv.notify_one(); return 0; }

inline int pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m) {
    std::unique_lock<std::mutex> lock(m->mu, std::adopt_lock);
    c->cv.wait(lock);
    lock.release();
    return 0;
}

inline int pthread_create(pthread_t *out, const void *, void *(*start)(void *), void *arg) {
    pthread_t thread = new nanogui_pthread;
    thread->thread = std::thread([start, arg] { start(arg); });
    *out = thread;
    return 0;
}

inline int pthread_join(pthread_t thread, void **) {
    if (thread && thread->thread.joinable())
        thread->thread.join();
    delete thread;
    return 0;
}

#endif
