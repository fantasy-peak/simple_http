#pragma once

// A pool of single-threaded io_contexts, each pinned to its own thread.
// Keeping one io_context per thread avoids handler synchronization inside a
// context and gives predictable per-connection affinity. Server pairs every
// context with one acceptor: a connection accepted on a worker is served on
// that same worker for its whole life.

#include <boost/asio.hpp>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace simple_http {

namespace asio = boost::asio;

class IoCtxPool final {
  public:
    explicit IoCtxPool(std::size_t pool_size) {
        if (pool_size == 0) {
            throw std::runtime_error("IoCtxPool size is 0");
        }
        for (std::size_t i = 0; i < pool_size; ++i) {
            create();
        }
    }

    // A started pool owns joinable threads, and std::thread's destructor
    // terminates the process while one is still joinable — so a pool destroyed
    // without an explicit stop() would abort. stop() is idempotent (it skips
    // threads that are already joined), which makes this safe either way.
    ~IoCtxPool() { stop(); }

    void start() {
        if (m_started)
            return; // idempotent: a second call must not spawn a second thread per context
        m_started = true;
        m_running = m_io_contexts.size();
        for (auto &context : m_io_contexts) {
            m_threads.emplace_back([this, ctx = context] {
                ctx->run();
                // This context has nothing left to do; stop() waits on this
                // before it forces anything.
                {
                    std::lock_guard lock(m_idle_mutex);
                    --m_running;
                }
                m_idle_cv.notify_all();
            });
        }
    }

    // How long stop() lets the contexts drain before it forces them. The drain
    // is normally over in microseconds — the caller has already cancelled
    // whatever was in flight — so this is the bound on a handler that will not
    // finish on its own, not the usual case.
    static constexpr auto kDrainGrace = std::chrono::seconds(2);

    // Stop every context and join its thread, draining first.
    //
    // The draining is the point. A pending composed operation is unwound *by*
    // its own completion handler: the inner read/write completes with
    // operation_aborted, the handler runs its cleanup, and the state it owns is
    // destroyed from a place that knows its invariants. Calling stop() on the
    // context first abandons that handler, and the state is then released from
    // the io_context's destructor instead — several layers away from anything
    // that knows what it holds.
    //
    // stop() remains the backstop: a context still busy after the grace period
    // is stopped the hard way, which is what makes a stuck handler a delayed
    // shutdown rather than a hung process.
    void stop() {
        m_work.clear();
        if (!m_threads.empty()) {
            std::unique_lock lock(m_idle_mutex);
            const bool drained = m_idle_cv.wait_for(lock, kDrainGrace, [this] { return m_running == 0; });
            if (!drained) {
                lock.unlock();
                for (auto &context : m_io_contexts) {
                    context->stop();
                }
            }
        }
        for (auto &thread : m_threads) {
            if (thread.joinable()) {
                thread.join();
            }
        }
    }

    // The pool is fixed at construction, so a reference stays valid for the
    // pool's lifetime.
    asio::io_context &at(std::size_t index) {
        assert(index < m_io_contexts.size());
        return *m_io_contexts[index];
    }

    std::size_t size() const { return m_io_contexts.size(); }

  private:
    void create() {
        auto ctx = std::make_shared<asio::io_context>(1);
        m_work.emplace_back(asio::make_work_guard(ctx->get_executor()));
        m_io_contexts.emplace_back(std::move(ctx));
    }

    std::vector<std::shared_ptr<asio::io_context>> m_io_contexts;
    std::vector<asio::executor_work_guard<asio::io_context::executor_type>> m_work;
    // Contexts still running their loop. Only meaningful once start() has
    // spawned the threads; a pool that was never started has nothing to wait for.
    std::mutex m_idle_mutex;
    std::condition_variable m_idle_cv;
    std::size_t m_running{0};
    std::vector<std::thread> m_threads;
    bool m_started{false};
};

} // namespace simple_http
