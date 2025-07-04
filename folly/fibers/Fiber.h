/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <functional>
#include <thread>
#include <typeinfo>

#include <folly/AtomicIntrusiveLinkedList.h>
#include <folly/CPortability.h>
#include <folly/Function.h>
#include <folly/IntrusiveList.h>
#include <folly/Portability.h>
#include <folly/fibers/BoostContextCompatibility.h>
#include <folly/io/async/Request.h>
#include <folly/lang/Thunk.h>
#include <folly/portability/PThread.h>

// include after CPortability.h defines this
#ifdef FOLLY_SANITIZE_THREAD
#include <sanitizer/tsan_interface.h>
#endif

namespace folly {
struct AsyncStackRoot;

namespace fibers {

class Baton;
class FiberManager;

struct TaskOptions {
  TaskOptions() {}
  /**
   * Should log the running time of the task? Refer to
   * getCurrentTaskRunningTime() for details.
   */
  bool logRunningTime = false;
};

/**
 * @class Fiber
 * @brief Fiber object used by FiberManager to execute tasks.
 *
 * Each Fiber object can be executing at most one task at a time. In active
 * phase it is running the task function and keeps its context.
 * Fiber is also used to pass data to blocked task and thus unblock it.
 * Each Fiber may be associated with a single FiberManager.
 */
class Fiber {
 public:
  /**
   * Resume the blocked task
   */
  void resume();

  Fiber(const Fiber&) = delete;
  Fiber& operator=(const Fiber&) = delete;

  ~Fiber();

  /**
   * Retrieve this fiber's base stack and stack size.
   *
   * @return This fiber's base stack pointer and stack size.
   */
  std::pair<void*, size_t> getStack() const {
    return {fiberStackLimit_, fiberStackSize_};
  }

  size_t stackHighWatermark() const { return fiberStackHighWatermark_; }

  folly::Optional<std::chrono::nanoseconds> getRunningTime() const;

  enum State : char {
    INVALID, /**< Does't have task function */
    NOT_STARTED, /**< Has task function, not started */
    READY_TO_RUN, /**< Was started, blocked, then unblocked */
    RUNNING, /**< Is running right now */
    AWAITING, /**< Is currently blocked */
    AWAITING_IMMEDIATE, /**< Was preempted to run an immediate function,
                             and will be resumed right away */
    YIELDED, /**< The fiber yielded execution voluntarily */
  };

  State getState() const { return state_; }

  /**
   * Retrieve this fiber's stack pointer. This is only meant for debugging.
   *
   * @return This fiber's current stack pointer.
   */
  void* getStackPointer() const { return fiberImpl_.getStackPointer(); }

 private:
  State state_{INVALID}; /**< current Fiber state */

  friend class Baton;
  friend class FiberManager;

  explicit Fiber(FiberManager& fiberManager);

  void init(bool recordStackUsed);

  template <typename F>
  void setFunction(F&& func, TaskOptions taskOptions);

  template <typename F, typename G>
  void setFunctionFinally(F&& func, G&& finally);

  [[noreturn]] void fiberFunc();

  /**
   * Switch out of fiber context into the main context,
   * performing necessary housekeeping for the new state.
   *
   * @param state New state, must not be RUNNING.
   */
  void preempt(State state);

  /**
   * Examines how much of the stack we used at this moment and
   * registers with the FiberManager (for monitoring).
   */
  void recordStackPosition();

  TaskOptions taskOptions_;
  bool recordStackUsed_{false};
  bool stackFilledWithMagic_{false};
  FiberManager& fiberManager_; /**< Associated FiberManager */
  size_t fiberStackSize_;
  size_t fiberStackHighWatermark_;
  unsigned char* fiberStackLimit_;
  FiberImpl fiberImpl_; /**< underlying fiber implementation */
  std::shared_ptr<RequestContext> rcontext_; /**< current RequestContext */
  folly::AsyncStackRoot* asyncRoot_ = nullptr;
  folly::Function<void()> func_; /**< task function */
  std::chrono::steady_clock::time_point currStartTime_;
  std::chrono::steady_clock::duration prevDuration_{0};
#ifdef FOLLY_SANITIZE_THREAD
  void* tsanCtx_{nullptr};
#endif

  /**
   * Points to next fiber in remote ready list
   */
  folly::AtomicIntrusiveLinkedListHook<Fiber> nextRemoteReady_;

  static constexpr size_t kUserBufferSize = 256;
  std::aligned_storage<kUserBufferSize>::type userBuffer_;

  void* getUserBuffer();

  folly::Function<void()> resultFunc_;
  folly::Function<void()> finallyFunc_;

  class LocalData {
   public:
    LocalData() {}
    ~LocalData();
    LocalData(const LocalData& other);
    LocalData& operator=(const LocalData& other);

    template <typename T>
    T& get() {
      if (data_) {
        assert(*vtable_.type == typeid(T));
        return *reinterpret_cast<T*>(data_);
      }
      return getSlow<T>();
    }

    void reset();

    // private:
    template <typename T>
    FOLLY_NOINLINE T& getSlow();

    static constexpr size_t kBufferSize = 128;
    using Buffer = std::aligned_storage<kBufferSize, cacheline_align_v>::type;
    struct VTable {
      std::type_info const* type;
      // on-heap
      void* (*make_copy)(void const*);
      void (*ruin)(void*);
      // in-situ
      void* (*ctor_copy)(void*, void const*);
      void (*dtor)(void*);

      template <typename T>
      static constexpr VTable get() noexcept {
        using t = folly::detail::thunk;
        return {
            &typeid(T),
            t::make_copy<T>,
            t::ruin<T>,
            t::ctor_copy<T>,
            t::dtor<T>};
      }
    };

    Buffer buffer_;
    VTable vtable_{};
    void* data_{nullptr};
  };

  LocalData localData_;

  folly::IntrusiveListHook listHook_; /**< list hook for different FiberManager
                                           queues */
  folly::IntrusiveListHook globalListHook_; /**< list hook for global list */

#ifdef FOLLY_SANITIZE_ADDRESS
  void* asanFakeStack_{nullptr};
  const void* asanMainStackBase_{nullptr};
  size_t asanMainStackSize_{0};
#endif
};
} // namespace fibers
} // namespace folly

#include <folly/fibers/Fiber-inl.h>
