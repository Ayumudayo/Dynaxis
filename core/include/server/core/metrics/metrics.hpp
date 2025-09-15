// UTF-8, 한국어 주석
#pragma once
#include <string>
#include <utility>
#include <initializer_list>
#include <memory>

namespace server::core::metrics {

using Label = std::pair<std::string, std::string>;
using Labels = std::initializer_list<Label>;

class Counter {
public:
    virtual ~Counter() = default;
    virtual void inc(double value = 1.0, Labels labels = {}) = 0;
};

class Gauge {
public:
    virtual ~Gauge() = default;
    virtual void set(double value, Labels labels = {}) = 0;
    virtual void inc(double value = 1.0, Labels labels = {}) = 0;
    virtual void dec(double value = 1.0, Labels labels = {}) = 0;
};

class Histogram {
public:
    virtual ~Histogram() = default;
    virtual void observe(double value, Labels labels = {}) = 0;
};

// 전역 레지스트리 접근자(현재는 no-op 구현 반환)
Counter& get_counter(const std::string& name);
Gauge& get_gauge(const std::string& name);
Histogram& get_histogram(const std::string& name);

} // namespace server::core::metrics

