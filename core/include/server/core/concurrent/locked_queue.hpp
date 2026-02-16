#pragma once

#include <cstddef>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <queue>
#include <utility>

namespace server::core::concurrent {

/**
 * @brief 제네릭 스레드 안전 큐 (Thread-safe Queue)
 * 
 * std::queue를 감싸서 Mutex로 보호하는 래퍼 클래스입니다.
 * 데이터 경쟁 없이 여러 스레드에서 안전하게 데이터를 넣고 뺄 수 있습니다.
 * 
 * @tparam T 큐에 저장할 데이터 타입
 */
template <typename T>
class LockedQueue {
public:
    using value_type = T;

    // capacity == 0 이면 무제한 큐로 동작합니다.
    // capacity > 0 이면 생산자 측 backpressure를 걸어 메모리 폭주를 완화합니다.
    explicit LockedQueue(std::size_t capacity = 0)
        : capacity_(capacity) {}

    LockedQueue(const LockedQueue&) = delete;
    LockedQueue& operator=(const LockedQueue&) = delete;

    void push(T item) {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [&]() { return capacity_ == 0 || queue_.size() < capacity_; });
        queue_.push(std::move(item));
        cv_.notify_one();
    }

    void push_swap(T& item) {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [&]() { return capacity_ == 0 || queue_.size() < capacity_; });
        queue_.emplace();
        std::swap(queue_.back(), item);
        cv_.notify_one();
    }

    void push_reset(T& item) {
        push_swap(item);
        item = T {};
    }

    std::optional<T> try_pop() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.empty()) {
            return std::nullopt;
        }
        std::optional<T> result(std::move(queue_.front()));
        queue_.pop();
        if (capacity_ > 0) {
            cv_.notify_one();
        }
        return result;
    }

    bool try_pop(T& out) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.empty()) {
            return false;
        }
        out = std::move(queue_.front());
        queue_.pop();
        if (capacity_ > 0) {
            cv_.notify_one();
        }
        return true;
    }

    bool try_push(T item) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (capacity_ > 0 && queue_.size() >= capacity_) {
            return false;
        }
        queue_.push(std::move(item));
        cv_.notify_one();
        return true;
    }

    std::size_t capacity() const {
        return capacity_;
    }

    bool empty() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.empty();
    }

    std::size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }

protected:
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::queue<T> queue_;
    std::size_t capacity_{0}; // 0이면 무제한
};

template <typename T>
class LockedWaitQueue : public LockedQueue<T> {
public:
    using typename LockedQueue<T>::value_type;

    explicit LockedWaitQueue(std::size_t capacity = 0)
        : LockedQueue<T>(capacity) {}

    LockedWaitQueue(const LockedWaitQueue&) = delete;
    LockedWaitQueue& operator=(const LockedWaitQueue&) = delete;

    // 소비자 스레드에서 호출하는 블로킹 pop입니다.
    // stop() 이후에는 nullopt를 반환해 워커 루프가 자연스럽게 빠져나오도록 설계했습니다.
    std::optional<T> pop_blocking() {
        std::unique_lock<std::mutex> lock(this->mutex_);
        this->cv_.wait(lock, [&]() { return !this->queue_.empty() || shutdown_; });
        if (this->queue_.empty()) {
            return std::nullopt;
        }
        std::optional<T> value(std::move(this->queue_.front()));
        this->queue_.pop();
        lock.unlock();
        if (this->capacity_ > 0) {
            this->cv_.notify_one();
        }
        return value;
    }

    // 생산자 push: bounded 큐에서는 공간이 생길 때까지 대기합니다.
    // stop() 이후에는 더 이상 큐에 넣지 않고 즉시 반환합니다.
    void push(T item) {
        std::unique_lock<std::mutex> lock(this->mutex_);
        this->cv_.wait(lock, [&]() {
            return shutdown_ || this->capacity_ == 0 || this->queue_.size() < this->capacity_;
        });
        if (shutdown_) {
            return;
        }
        this->queue_.push(std::move(item));
        this->cv_.notify_one();
    }

    // 대기 중인 생산자/소비자를 모두 깨워 종료 경로를 빠르게 만든다.
    void stop() {
        std::lock_guard<std::mutex> lock(this->mutex_);
        shutdown_ = true;
        this->cv_.notify_all();
    }

    // 테스트/재사용 시 stop 상태를 되돌려 다시 push/pop을 허용한다.
    void reset() {
        std::lock_guard<std::mutex> lock(this->mutex_);
        shutdown_ = false;
    }

private:
    bool shutdown_ = false;
};

} // namespace server::core::concurrent
