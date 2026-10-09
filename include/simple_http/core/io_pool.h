#pragma once

// A pool of single-threaded io_contexts, each pinned to its own thread. Work is
// distributed round-robin. Keeping one io_context per thread avoids handler
// synchronization inside a context and gives predictable per-connection
// affinity. A "main" context, created last, can be added for acceptors when the
// accept topology needs a thread of its own (Server does this unless the
// listener is fanned out with SO_REUSEPORT).

#include <atomic>
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
    explicit IoCtxPool(std::size_t pool_size) : m_pool_size(pool_size) {
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
    // The draining is the point. A connection coroutine parked on a read is
    // unwound *by* its own completion handler: the read completes with
    // operation_aborted, the coroutine runs to its end, and the frame is
    // destroyed from a place that knows its invariants. Calling stop() on the
    // context first abandons that handler, and the frame is then destroyed from
    // the io_context's destructor instead — several layers from anything that
    // knows what the frame holds. For an engine that races two child coroutines
    // with `operator||`, that destruction walks state the operator's own
    // teardown has already released.
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

    // Round-robin selection of the next worker io_context.
    asio::io_context &next() { return *next_ptr(); }

    // The same, as the shared_ptr. A *const* reference: handing out a mutable one
    // let any caller rebind a pooled context, which left "the main context is
    // never handed out as a worker" a comment rather than a guarantee.
    const std::shared_ptr<asio::io_context> &next_ptr() {
        std::size_t index = m_cursor.fetch_add(1, std::memory_order_relaxed);
        return m_io_contexts[index % m_pool_size];
    }

    // The context reserved for acceptors (added via add_main_context()).
    std::shared_ptr<asio::io_context> &main_context() { return m_io_contexts.back(); }

    // The worker contexts only: a main context, if one was added, is not
    // counted here and is never reachable through at().
    std::size_t size() const { return m_pool_size; }

    // Bounds-checked, as the name implies. An out-of-range index used to read
    // past the end — or, for index == pool_size, hand back the main context,
    // which is precisely the one this accessor exists to keep away from workers.
    const std::shared_ptr<asio::io_context> &at(std::size_t index) {
        assert(index < m_pool_size);
        return m_io_contexts[index];
    }

    void add_main_context() { create(); }

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
    std::atomic_uint64_t m_cursor{0};
    std::vector<std::thread> m_threads;
    std::size_t m_pool_size;
};

} // namespace simple_http
