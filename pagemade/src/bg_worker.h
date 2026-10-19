/*
 * pagemade/bg_worker.h — one background thread that runs jobs in order.
 *
 * The thread is detached, so an owner never waits on a slow read (a file on
 * a network share) when it goes away: stop() drops the queue, and the thread
 * finishes the job in hand and exits. A job posts its own result back to the
 * UI thread (nanogui::async) and checks there that its owner still exists.
 *
 * A job with a key replaces a queued job with the same key, so the newest
 * request for one thing wins (the picture the user just selected, the
 * display size the view now wants).
 *
 * With `threads` above 1, that many threads take jobs from the one queue,
 * so a slow job doesn't hold up the next. Jobs then run in queue order but
 * can finish out of it.
 */
#pragma once

#include <condition_variable>
#include <cstdio>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <utility>

namespace pagemade {

class BackgroundWorker {
public:
    explicit BackgroundWorker(int threads = 1) : m_state(std::make_shared<State>()) {
        m_state->max = threads < 1 ? 1 : threads;
    }
    ~BackgroundWorker() { stop(); }
    BackgroundWorker(const BackgroundWorker &) = delete;
    BackgroundWorker &operator=(const BackgroundWorker &) = delete;

    void post(std::function<void()> job, const std::string &key = std::string()) {
        std::lock_guard<std::mutex> lock(m_state->mu);
        if (m_state->stopped)
            return;
        /* A replace used to return here. If the first std::thread
         * constructor threw, a later post of the same key replaced the
         * job and left running false forever: the spawn retry is below. */
        bool replaced = false;
        if (!key.empty())
            for (auto &q : m_state->queue)
                if (q.first == key) {
                    q.second = std::move(job);
                    replaced = true;
                    break;
                }
        if (!replaced)
            m_state->queue.emplace_back(key, std::move(job));
        /* Another thread when none is waiting for work and there is room */
        if (m_state->idle == 0 && m_state->running < m_state->max) {
            try {
                std::thread t(run, m_state);
                t.detach();
                ++m_state->running;        // only once the thread exists
            } catch (const std::system_error &) {
                /* Out of threads: the job stays queued and the next post()
                 * tries to spawn a worker again (a running one still takes it). */
            }
        }
        m_state->cv.notify_one();
    }

    /* Take queued jobs whose key matches out of the queue. */
    void drop_if(const std::function<bool(const std::string &key)> &pred) {
        std::lock_guard<std::mutex> lock(m_state->mu);
        for (auto it = m_state->queue.begin(); it != m_state->queue.end();)
            it = pred(it->first) ? m_state->queue.erase(it) : it + 1;
    }

    bool queued(const std::string &key) const {
        std::lock_guard<std::mutex> lock(m_state->mu);
        for (const auto &q : m_state->queue)
            if (q.first == key)
                return true;
        return false;
    }

    void stop() {
        std::lock_guard<std::mutex> lock(m_state->mu);
        m_state->stopped = true;
        m_state->queue.clear();
        m_state->cv.notify_all();
    }

private:
    struct State {
        mutable std::mutex mu;
        std::condition_variable cv;
        std::deque<std::pair<std::string, std::function<void()>>> queue;
        int running = 0, idle = 0, max = 1;
        bool stopped = false;
    };

    static void run(std::shared_ptr<State> state) {
        std::unique_lock<std::mutex> lock(state->mu);
        for (;;) {
            ++state->idle;
            state->cv.wait(lock, [&] { return state->stopped || !state->queue.empty(); });
            --state->idle;
            if (state->stopped)
                return;
            std::function<void()> job = std::move(state->queue.front().second);
            state->queue.pop_front();
            lock.unlock();
            try {
                job();
            } catch (...) {
                /* A failed job must not take the process down with it:
                 * an exception escaping a thread is std::terminate. */
                std::fprintf(stderr, "pagemade: a background job threw and was dropped\n");
            }
            job = nullptr;               // captured state goes before the lock
            lock.lock();
        }
    }

    std::shared_ptr<State> m_state;
};

} // namespace pagemade
