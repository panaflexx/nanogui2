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
    BackgroundWorker() : m_state(std::make_shared<State>()) { }
    ~BackgroundWorker() { stop(); }
    BackgroundWorker(const BackgroundWorker &) = delete;
    BackgroundWorker &operator=(const BackgroundWorker &) = delete;

    void post(std::function<void()> job, const std::string &key = std::string()) {
        std::lock_guard<std::mutex> lock(m_state->mu);
        if (m_state->stopped)
            return;
        if (!key.empty())
            for (auto &q : m_state->queue)
                if (q.first == key) {
                    q.second = std::move(job);
                    return;
                }
        m_state->queue.emplace_back(key, std::move(job));
        if (!m_state->running) {
            try {
                std::thread t(run, m_state);
                t.detach();
                m_state->running = true;   // only once the thread exists
            } catch (const std::system_error &) {
                /* Out of threads: the job stays queued and the next post()
                 * tries to spawn the worker again. */
                return;
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
        m_state->cv.notify_one();
    }

private:
    struct State {
        mutable std::mutex mu;
        std::condition_variable cv;
        std::deque<std::pair<std::string, std::function<void()>>> queue;
        bool running = false, stopped = false;
    };

    static void run(std::shared_ptr<State> state) {
        std::unique_lock<std::mutex> lock(state->mu);
        for (;;) {
            state->cv.wait(lock, [&] { return state->stopped || !state->queue.empty(); });
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
