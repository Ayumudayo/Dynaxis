#include "server/core/metrics/metrics.hpp"
#include <unordered_map>
#include <mutex>

namespace server::core::metrics {

namespace {
struct NoopCounter final : Counter { void inc(double, Labels) override {} };
struct NoopGauge final : Gauge { void set(double, Labels) override {} void inc(double, Labels) override {} void dec(double, Labels) override {} };
struct NoopHistogram final : Histogram { void observe(double, Labels) override {} };

std::mutex& mu() { static std::mutex m; return m; }
std::unordered_map<std::string, NoopCounter>& counters() { static std::unordered_map<std::string, NoopCounter> m; return m; }
std::unordered_map<std::string, NoopGauge>& gauges() { static std::unordered_map<std::string, NoopGauge> m; return m; }
std::unordered_map<std::string, NoopHistogram>& histos() { static std::unordered_map<std::string, NoopHistogram> m; return m; }
}

// 아직 Prometheus와 통합되지 않은 환경에서도 동일 API를 호출할 수 있도록
// Noop 객체를 리턴한다. 실제 exporter가 연결되면 이 구현을 대체한다.
Counter& get_counter(const std::string& name) {
    std::lock_guard<std::mutex> lk(mu());
    return counters().try_emplace(name).first->second;
}

Gauge& get_gauge(const std::string& name) {
    std::lock_guard<std::mutex> lk(mu());
    return gauges().try_emplace(name).first->second;
}

Histogram& get_histogram(const std::string& name) {
    std::lock_guard<std::mutex> lk(mu());
    return histos().try_emplace(name).first->second;
}

} // namespace server::core::metrics
