#include <gtest/gtest.h>
#include <server/core/metrics/metrics.hpp>

using namespace server::core::metrics;

// Metrics API 기본 동작 테스트
TEST(MetricsTest, BasicApi) {
    // 카운터 가져오기 (구현체가 없으면 no-op 객체 반환)
    Counter& counter = get_counter("test_counter");
    counter.inc(1.0); // 충돌 없이 호출되는지 확인

    // 게이지 가져오기
    Gauge& gauge = get_gauge("test_gauge");
    gauge.set(10.0);
    gauge.inc(1.0);
    gauge.dec(1.0);

    // 히스토그램 가져오기
    Histogram& histogram = get_histogram("test_histogram");
    histogram.observe(5.0);
    
    // 현재는 실제 메트릭 수집 여부를 확인할 방법이 없으므로(no-op),
    // API 호출 시 크래시가 발생하지 않는지 위주로 검증합니다.
    SUCCEED();
}
