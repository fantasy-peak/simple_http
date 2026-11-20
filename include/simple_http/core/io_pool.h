#pragma once

// A pool of single-threaded io_contexts, each pinned to its own thread.
// Keeping one io_context per thread avoids handler synchronization inside a
// context and gives predictable per-connection affinity. Server pairs every
// context with one acceptor: a connection accepted on a worker is served on
// that same worker for its whole life.

#include <algorithm>
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
    IoCtxPool(const IoCtxPool &) = delete;
    IoCtxPool &operator=(const IoCtxPool &) = delete;
    IoCtxPool(IoCtxPool &&) = delete;
    IoCtxPool &operator=(IoCtxPool &&) = delete;

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
    ~IoCtxPool() {
        if (on_worker_thread()) {
            // A worker cannot join itself. Teardown from a handler is not the
            // supported path (Server::stop() refuses it too), but the
            // destructor must still not throw or terminate: release the work
            // guards, stop the contexts, join the siblings and detach this
            // thread. The thread body only touches the shared idle state and
            // its captured context, both of which outlive the pool object.
            m_work.clear();
            for (auto &context : m_io_contexts) {
                context->stop();
            }
            const auto self = std::this_thread::get_id();
            for (auto &thread : m_threads) {
                if (!thread.joinable()) {
                    continue;
                }
                if (thread.get_id() == self) {
                    thread.detach();
                } else {
                    thread.join();
                }
            }
            return;
        }
        stop();
    }

    void start() {
        if (m_started) {
            return; // idempotent: a second call must not spawn a second thread per context
        }
        m_started = true;
        m_idle->running = m_io_contexts.size();
        m_thread_ids.assign(m_io_contexts.size(), std::thread::id{});
        for (std::size_t i = 0, n = m_io_contexts.size(); i < n; ++i) {
            auto context = m_io_contexts.at(i);
            m_threads.emplace_back([this, context, i] {
                // Record the id before run(): stop() may be called from this
                // very thread (a handler) and must recognise it.
                {
                    std::scoped_lock lock(m_ids_mutex);
                    m_thread_ids.at(i) = std::this_thread::get_id();
                }
                context->run();
                // This context has nothing left to do; stop() waits on this
                // before it forces anything. Only the shared state is touched
                // here, never `this`: the pool may already have been destroyed.
                {
                    std::scoped_lock lock(m_idle->mutex);
                    --m_idle->running;
                }
                m_idle->cv.notify_all();
            });
        }
    }

    // Whether the calling thread is one of this pool's workers. Server::stop()
    // uses it to refuse teardown from a connection thread instead of joining
    // itself (which throws std::system_error and aborts the process).
    [[nodiscard]] bool on_worker_thread() const noexcept {
        const auto self = std::this_thread::get_id();
        std::scoped_lock lock(m_ids_mutex);
        return std::any_of(m_thread_ids.begin(), m_thread_ids.end(),
                           [self](const std::thread::id id) noexcept { return id == self; });
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
        // Joining the calling thread throws std::system_error
        // (resource_deadlock_would_occur), which escapes io_context::run() and
        // terminates the process. Teardown from a connection handler must be
        // posted to another thread; until then this is a no-op.
        if (on_worker_thread()) {
            return;
        }
        m_work.clear();
        if (!m_threads.empty()) {
            std::unique_lock lock(m_idle->mutex);
            const bool drained = m_idle->cv.wait_for(lock, kDrainGrace, [this] { return m_idle->running == 0; });
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
    asio::io_context &at(std::size_t index) { return *m_io_contexts.at(index); }

    std::size_t size() const { return m_io_contexts.size(); }

  private:
    void create() {
        auto ctx = std::make_shared<asio::io_context>(1);
        m_work.emplace_back(asio::make_work_guard(ctx->get_executor()));
        m_io_contexts.emplace_back(std::move(ctx));
    }

    // Shared with every worker thread body so that the body never touches the
    // pool object after run() returns.
    struct IdleState {
        std::mutex mutex;
        std::condition_variable cv;
        std::size_t running{0};
    };

    std::vector<std::shared_ptr<asio::io_context>> m_io_contexts;
    std::vector<asio::executor_work_guard<asio::io_context::executor_type>> m_work;
    // Contexts still running their loop. Only meaningful once start() has
    // spawned the threads; a pool that was never started has nothing to wait for.
    std::shared_ptr<IdleState> m_idle{std::make_shared<IdleState>()};
    // Worker thread ids, written by each thread as it starts and read by
    // on_worker_thread(). Guarded because start() may race the first stop().
    mutable std::mutex m_ids_mutex;
    std::vector<std::thread::id> m_thread_ids;
    std::vector<std::thread> m_threads;
    bool m_started{false};
};

} // namespace simple_http
