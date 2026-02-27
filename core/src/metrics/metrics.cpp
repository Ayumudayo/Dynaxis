#include "server/core/metrics/metrics.hpp"
#include "server/core/runtime_metrics.hpp"

#include <algorithm>
#include <array>
#include <iomanip>
#include <memory>
#include <mutex>
#include <sstream>
#include <string_view>
#include <unordered_map>
#include <vector>

/**
 * @brief 공용 metrics backend 구현입니다.
 *
 * Counter/Gauge/Histogram 호출 값을 프로세스 내 registry에 축적하고,
 * Prometheus text 형식으로 직렬화해 `/metrics` 응답에 합성할 수 있도록 제공합니다.
 */
namespace server::core::metrics {

namespace {

std::string escape_prometheus_label_value(std::string_view value) {
    std::string out;
    out.reserve(value.size());
    for (const char ch : value) {
        switch (ch) {
        case '\\': out += "\\\\"; break;
        case '"': out += "\\\""; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default: out.push_back(ch); break;
        }
    }
    return out;
}

std::vector<Label> normalize_labels(Labels labels) {
    std::vector<Label> normalized;
    normalized.reserve(labels.size());
    for (const auto& [key, value] : labels) {
        normalized.emplace_back(key, value);
    }
    std::sort(normalized.begin(), normalized.end(), [](const Label& lhs, const Label& rhs) {
        if (lhs.first == rhs.first) {
            return lhs.second < rhs.second;
        }
        return lhs.first < rhs.first;
    });
    return normalized;
}

std::string make_label_key(const std::vector<Label>& labels) {
    std::string key;
    for (const auto& [name, value] : labels) {
        key += name;
        key.push_back('\x1f');
        key += value;
        key.push_back('\x1e');
    }
    return key;
}

std::string make_label_text(const std::vector<Label>& labels) {
    std::ostringstream stream;
    bool first = true;
    for (const auto& [name, value] : labels) {
        if (!first) {
            stream << ',';
        }
        first = false;
        stream << name << "=\"" << escape_prometheus_label_value(value) << '\"';
    }
    return stream.str();
}

std::string format_double(double value) {
    std::ostringstream stream;
    stream << std::setprecision(15) << value;
    return stream.str();
}

void append_sample_line(std::ostream& out,
                        std::string_view metric_name,
                        std::string_view labels,
                        std::string_view value) {
    out << metric_name;
    if (!labels.empty()) {
        out << '{' << labels << '}';
    }
    out << ' ' << value << '\n';
}

struct CounterSeries {
    double value{0.0};
    std::string labels;
};

struct GaugeSeries {
    double value{0.0};
    std::string labels;
};

struct HistogramSeries {
    std::vector<std::uint64_t> bucket_counts;
    std::uint64_t count{0};
    double sum{0.0};
    std::string labels;

    HistogramSeries()
        : bucket_counts(10, 0) {
    }
};

inline constexpr std::array<double, 10> kHistogramUpperBounds = {
    0.1,
    0.5,
    1.0,
    2.5,
    5.0,
    10.0,
    25.0,
    50.0,
    100.0,
    250.0,
};

class CounterImpl final : public Counter {
public:
    explicit CounterImpl(std::string name)
        : name_(std::move(name)) {
    }

    const std::string& name() const {
        return name_;
    }

    void inc(double value, Labels labels) override {
        if (value < 0.0) {
            return;
        }
        const auto normalized = normalize_labels(labels);
        const auto key = make_label_key(normalized);
        const auto label_text = make_label_text(normalized);

        std::lock_guard<std::mutex> lock(mu_);
        auto& series = series_[key];
        series.value += value;
        if (series.labels.empty()) {
            series.labels = label_text;
        }
    }

    void append(std::ostream& out) const {
        std::vector<std::pair<std::string, CounterSeries>> rows;
        {
            std::lock_guard<std::mutex> lock(mu_);
            rows.reserve(series_.size());
            for (const auto& [key, series] : series_) {
                rows.emplace_back(key, series);
            }
        }
        std::sort(rows.begin(), rows.end(), [](const auto& lhs, const auto& rhs) {
            return lhs.first < rhs.first;
        });

        out << "# TYPE " << name_ << " counter\n";
        for (const auto& [_, series] : rows) {
            append_sample_line(out, name_, series.labels, format_double(series.value));
        }
    }

private:
    std::string name_;
    mutable std::mutex mu_;
    std::unordered_map<std::string, CounterSeries> series_;
};

class GaugeImpl final : public Gauge {
public:
    explicit GaugeImpl(std::string name)
        : name_(std::move(name)) {
    }

    const std::string& name() const {
        return name_;
    }

    void set(double value, Labels labels) override {
        const auto normalized = normalize_labels(labels);
        const auto key = make_label_key(normalized);
        const auto label_text = make_label_text(normalized);

        std::lock_guard<std::mutex> lock(mu_);
        auto& series = series_[key];
        series.value = value;
        if (series.labels.empty()) {
            series.labels = label_text;
        }
    }

    void inc(double value, Labels labels) override {
        const auto normalized = normalize_labels(labels);
        const auto key = make_label_key(normalized);
        const auto label_text = make_label_text(normalized);

        std::lock_guard<std::mutex> lock(mu_);
        auto& series = series_[key];
        series.value += value;
        if (series.labels.empty()) {
            series.labels = label_text;
        }
    }

    void dec(double value, Labels labels) override {
        const auto normalized = normalize_labels(labels);
        const auto key = make_label_key(normalized);
        const auto label_text = make_label_text(normalized);

        std::lock_guard<std::mutex> lock(mu_);
        auto& series = series_[key];
        series.value -= value;
        if (series.labels.empty()) {
            series.labels = label_text;
        }
    }

    void append(std::ostream& out) const {
        std::vector<std::pair<std::string, GaugeSeries>> rows;
        {
            std::lock_guard<std::mutex> lock(mu_);
            rows.reserve(series_.size());
            for (const auto& [key, series] : series_) {
                rows.emplace_back(key, series);
            }
        }
        std::sort(rows.begin(), rows.end(), [](const auto& lhs, const auto& rhs) {
            return lhs.first < rhs.first;
        });

        out << "# TYPE " << name_ << " gauge\n";
        for (const auto& [_, series] : rows) {
            append_sample_line(out, name_, series.labels, format_double(series.value));
        }
    }

private:
    std::string name_;
    mutable std::mutex mu_;
    std::unordered_map<std::string, GaugeSeries> series_;
};

class HistogramImpl final : public Histogram {
public:
    explicit HistogramImpl(std::string name)
        : name_(std::move(name)) {
    }

    const std::string& name() const {
        return name_;
    }

    void observe(double value, Labels labels) override {
        const auto normalized = normalize_labels(labels);
        const auto key = make_label_key(normalized);
        const auto label_text = make_label_text(normalized);

        std::lock_guard<std::mutex> lock(mu_);
        auto& series = series_[key];
        if (series.labels.empty()) {
            series.labels = label_text;
        }

        series.sum += value;
        ++series.count;

        for (std::size_t i = 0; i < kHistogramUpperBounds.size(); ++i) {
            if (value <= kHistogramUpperBounds[i]) {
                ++series.bucket_counts[i];
            }
        }
    }

    void append(std::ostream& out) const {
        std::vector<std::pair<std::string, HistogramSeries>> rows;
        {
            std::lock_guard<std::mutex> lock(mu_);
            rows.reserve(series_.size());
            for (const auto& [key, series] : series_) {
                rows.emplace_back(key, series);
            }
        }
        std::sort(rows.begin(), rows.end(), [](const auto& lhs, const auto& rhs) {
            return lhs.first < rhs.first;
        });

        out << "# TYPE " << name_ << " histogram\n";
        for (const auto& [_, series] : rows) {
            for (std::size_t i = 0; i < kHistogramUpperBounds.size(); ++i) {
                std::string labels = series.labels;
                if (!labels.empty()) {
                    labels += ',';
                }
                labels += "le=\"" + format_double(kHistogramUpperBounds[i]) + "\"";
                append_sample_line(out, name_ + "_bucket", labels, std::to_string(series.bucket_counts[i]));
            }

            std::string inf_labels = series.labels;
            if (!inf_labels.empty()) {
                inf_labels += ',';
            }
            inf_labels += "le=\"+Inf\"";
            append_sample_line(out, name_ + "_bucket", inf_labels, std::to_string(series.count));

            append_sample_line(out, name_ + "_sum", series.labels, format_double(series.sum));
            append_sample_line(out, name_ + "_count", series.labels, std::to_string(series.count));
        }
    }

private:
    std::string name_;
    mutable std::mutex mu_;
    std::unordered_map<std::string, HistogramSeries> series_;
};

using CounterMap = std::unordered_map<std::string, std::unique_ptr<CounterImpl>>;
using GaugeMap = std::unordered_map<std::string, std::unique_ptr<GaugeImpl>>;
using HistogramMap = std::unordered_map<std::string, std::unique_ptr<HistogramImpl>>;

std::mutex& registry_mu() {
    static std::mutex m;
    return m;
}

CounterMap& counter_registry() {
    static CounterMap map;
    return map;
}

GaugeMap& gauge_registry() {
    static GaugeMap map;
    return map;
}

HistogramMap& histogram_registry() {
    static HistogramMap map;
    return map;
}

}

Counter& get_counter(const std::string& name) {
    std::lock_guard<std::mutex> lock(registry_mu());
    auto& registry = counter_registry();
    auto it = registry.find(name);
    if (it == registry.end()) {
        it = registry.emplace(name, std::make_unique<CounterImpl>(name)).first;
    }
    return *it->second;
}

Gauge& get_gauge(const std::string& name) {
    std::lock_guard<std::mutex> lock(registry_mu());
    auto& registry = gauge_registry();
    auto it = registry.find(name);
    if (it == registry.end()) {
        it = registry.emplace(name, std::make_unique<GaugeImpl>(name)).first;
    }
    return *it->second;
}

Histogram& get_histogram(const std::string& name) {
    std::lock_guard<std::mutex> lock(registry_mu());
    auto& registry = histogram_registry();
    auto it = registry.find(name);
    if (it == registry.end()) {
        it = registry.emplace(name, std::make_unique<HistogramImpl>(name)).first;
    }
    return *it->second;
}

void append_prometheus_metrics(std::ostream& out) {
    std::vector<CounterImpl*> counters;
    std::vector<GaugeImpl*> gauges;
    std::vector<HistogramImpl*> histograms;
    {
        std::lock_guard<std::mutex> lock(registry_mu());
        counters.reserve(counter_registry().size());
        for (auto& [_, metric] : counter_registry()) {
            counters.push_back(metric.get());
        }

        gauges.reserve(gauge_registry().size());
        for (auto& [_, metric] : gauge_registry()) {
            gauges.push_back(metric.get());
        }

        histograms.reserve(histogram_registry().size());
        for (auto& [_, metric] : histogram_registry()) {
            histograms.push_back(metric.get());
        }
    }

    std::sort(counters.begin(), counters.end(), [](const CounterImpl* lhs, const CounterImpl* rhs) {
        return lhs->name() < rhs->name();
    });
    std::sort(gauges.begin(), gauges.end(), [](const GaugeImpl* lhs, const GaugeImpl* rhs) {
        return lhs->name() < rhs->name();
    });
    std::sort(histograms.begin(), histograms.end(), [](const HistogramImpl* lhs, const HistogramImpl* rhs) {
        return lhs->name() < rhs->name();
    });

    for (const auto* metric : counters) {
        metric->append(out);
    }
    for (const auto* metric : gauges) {
        metric->append(out);
    }
    for (const auto* metric : histograms) {
        metric->append(out);
    }
}

void append_runtime_core_metrics(std::ostream& out) {
    const auto snap = server::core::runtime_metrics::snapshot();

    out << "# TYPE core_runtime_accept_total counter\n";
    out << "core_runtime_accept_total " << snap.accept_total << "\n";

    out << "# TYPE core_runtime_session_started_total counter\n";
    out << "core_runtime_session_started_total " << snap.session_started_total << "\n";

    out << "# TYPE core_runtime_session_stopped_total counter\n";
    out << "core_runtime_session_stopped_total " << snap.session_stopped_total << "\n";

    out << "# TYPE core_runtime_session_active gauge\n";
    out << "core_runtime_session_active " << snap.session_active << "\n";

    out << "# TYPE core_runtime_dispatch_total counter\n";
    out << "core_runtime_dispatch_total " << snap.dispatch_total << "\n";

    out << "# TYPE core_runtime_dispatch_unknown_total counter\n";
    out << "core_runtime_dispatch_unknown_total " << snap.dispatch_unknown_total << "\n";

    out << "# TYPE core_runtime_dispatch_exception_total counter\n";
    out << "core_runtime_dispatch_exception_total " << snap.dispatch_exception_total << "\n";

    out << "# TYPE core_runtime_send_queue_drop_total counter\n";
    out << "core_runtime_send_queue_drop_total " << snap.send_queue_drop_total << "\n";

    out << "# TYPE core_runtime_packet_error_total counter\n";
    out << "core_runtime_packet_error_total " << snap.packet_error_total << "\n";
}

void reset_for_tests() {
    std::lock_guard<std::mutex> lock(registry_mu());
    counter_registry().clear();
    gauge_registry().clear();
    histogram_registry().clear();
}

} // namespace server::core::metrics
