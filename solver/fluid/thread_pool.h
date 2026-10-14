// Small persistent worker pool for Fluid::setParallelFor (header-only, std::thread). run() may be called from any
// thread, also from inside a chunk of another run (nested: the shim steps several fluids in parallel and each
// fluid's collide splits again). Each run is a job with its own chunk counter; the caller works on its own job and,
// while chunks claimed by others are still running, helps with other open jobs of the same group, so nesting cannot
// deadlock (a thread only ever waits for chunks that are already being executed). n threads = n - 1 workers + the
// caller. A group is a top-level run and the runs nested in its chunks: a caller does not help other groups, so the
// game thread waiting on a small fluid step does not pick up a cloth job's chunk (a whole cloth step, milliseconds)
// that another thread started. Idle workers take chunks of any group.
#pragma once

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <thread>
#include <vector>

namespace pxr {

class ThreadPool {
 public:
  explicit ThreadPool(unsigned threads) {
    for (unsigned i = 1; i < threads; ++i) workers_.emplace_back([this] { loop(); });
  }
  ~ThreadPool() {
    {
      std::lock_guard<std::mutex> l(m_);
      quit_ = true;
    }
    cv_.notify_all();
    for (std::thread& t : workers_) t.join();
  }
  unsigned threads() const { return (unsigned)workers_.size() + 1; }

  void run(size_t n, void (*fn)(void*, size_t), void* ctx) {
    if (n == 0) return;
    if (workers_.empty() || n == 1) {
      for (size_t c = 0; c < n; ++c) fn(ctx, c);
      return;
    }
    Job job;
    job.fn = fn, job.ctx = ctx, job.n = n;
    job.group = tlsGroup() ? tlsGroup() : &job;  // nested runs join the group of the chunk they run in
    {
      std::lock_guard<std::mutex> l(m_);
      open_.push_back(&job);
    }
    cv_.notify_all();
    work(job);  // the caller's own chunks first
    for (;;) {  // then wait for the chunks others claimed, helping with other jobs meanwhile
      if (job.done.load() == n) break;
      Job* other = nullptr;
      {
        std::unique_lock<std::mutex> l(m_);
        if (job.done.load() == n) break;
        other = pick(job.group);
        if (!other) {
          done_.wait(l, [&] { return job.done.load() == n || pick(job.group) != nullptr; });
          continue;
        }
        ++other->users;
      }
      work(*other);
      release(*other);
    }
    // nobody may still hold the job (a worker between picking it and finding no chunk left)
    std::unique_lock<std::mutex> l(m_);
    open_.erase(std::find(open_.begin(), open_.end(), &job));
    done_.wait(l, [&] { return job.users == 0; });
  }

 private:
  struct Job {
    void (*fn)(void*, size_t) = nullptr;
    void* ctx = nullptr;
    size_t n = 0;
    std::atomic<size_t> next{0}, done{0};
    unsigned users = 0;  // threads working on it besides its owner (guarded by m_)
    const void* group = nullptr;
  };
  // The group of the chunk this thread is running (null outside any chunk).
  static const void*& tlsGroup() {
    static thread_local const void* g = nullptr;
    return g;
  }
  // An open job with chunks left, of the given group or of any (m_ held).
  Job* pick(const void* group = nullptr) {
    for (Job* j : open_)
      if (j->next.load() < j->n && (!group || j->group == group)) return j;
    return nullptr;
  }
  void work(Job& job) {
    size_t c, finished = 0;
    const void* outer = tlsGroup();
    tlsGroup() = job.group;
    while ((c = job.next.fetch_add(1)) < job.n) {
      job.fn(job.ctx, c);
      ++finished;
    }
    tlsGroup() = outer;
    if (finished && job.done.fetch_add(finished) + finished == job.n) {
      std::lock_guard<std::mutex> l(m_);
      done_.notify_all();
    }
  }
  void release(Job& job) {
    std::lock_guard<std::mutex> l(m_);
    if (--job.users == 0) done_.notify_all();
  }
  void loop() {
    for (;;) {
      Job* job;
      {
        std::unique_lock<std::mutex> l(m_);
        // (chunk counters move without the lock: keep the job the predicate found)
        job = nullptr;
        cv_.wait(l, [&] { return quit_ || (job = pick()) != nullptr; });
        if (quit_) return;
        ++job->users;
      }
      work(*job);
      release(*job);
    }
  }

  std::vector<std::thread> workers_;
  std::mutex m_;
  std::condition_variable cv_, done_;
  std::vector<Job*> open_;
  bool quit_ = false;
};

}  // namespace pxr
