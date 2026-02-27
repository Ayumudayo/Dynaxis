#pragma once

#include <initializer_list>
#include <ostream>
#include <string>
#include <utility>

namespace server::core::metrics {

/** @brief 메트릭 라벨 키/값 쌍입니다. */
using Label = std::pair<std::string, std::string>;
/** @brief 메트릭 호출 시 전달하는 라벨 목록 타입입니다. */
using Labels = std::initializer_list<Label>;

/** @brief 단조 증가 카운터 메트릭 인터페이스입니다. */
class Counter {
public:
    virtual ~Counter() = default;
    /**
     * @brief 카운터를 증가시킵니다.
     * @param value 증가량
     * @param labels 라벨 목록
     */
    virtual void inc(double value = 1.0, Labels labels = {}) = 0;
};

/** @brief 증감 가능한 게이지 메트릭 인터페이스입니다. */
class Gauge {
public:
    virtual ~Gauge() = default;
    /** @brief 값을 절대치로 설정합니다. */
    virtual void set(double value, Labels labels = {}) = 0;
    /** @brief 값을 증가시킵니다. */
    virtual void inc(double value = 1.0, Labels labels = {}) = 0;
    /** @brief 값을 감소시킵니다. */
    virtual void dec(double value = 1.0, Labels labels = {}) = 0;
};

/** @brief 분포 관측용 히스토그램 메트릭 인터페이스입니다. */
class Histogram {
public:
    virtual ~Histogram() = default;
    /** @brief 관측값 1건을 기록합니다. */
    virtual void observe(double value, Labels labels = {}) = 0;
};

/**
 * @brief 이름으로 카운터를 조회합니다.
 * @param name 메트릭 이름
 * @return 해당 이름의 카운터 객체 레퍼런스
 *
 * 계약:
 * - 동일 name 요청은 동일 객체 레퍼런스를 반환합니다.
 * - 내부 registry 백엔드에 즉시 반영되며 호출은 예외 없이 동작합니다.
 */
Counter& get_counter(const std::string& name);

/**
 * @brief 이름으로 게이지를 조회합니다.
 * @param name 메트릭 이름
 * @return 해당 이름의 게이지 객체 레퍼런스
 *
 * 계약:
 * - 동일 name 요청은 동일 객체 레퍼런스를 반환합니다.
 * - 내부 registry 백엔드에 즉시 반영되며 호출은 예외 없이 동작합니다.
 */
Gauge& get_gauge(const std::string& name);

/**
 * @brief 이름으로 히스토그램을 조회합니다.
 * @param name 메트릭 이름
 * @return 해당 이름의 히스토그램 객체 레퍼런스
 *
 * 계약:
 * - 동일 name 요청은 동일 객체 레퍼런스를 반환합니다.
 * - 내부 registry 백엔드에 즉시 반영되며 호출은 예외 없이 동작합니다.
 */
Histogram& get_histogram(const std::string& name);

/**
 * @brief 등록된 공용 metrics backend 값을 Prometheus text 형식으로 출력합니다.
 *
 * 출력은 `# TYPE` + 샘플 라인으로 구성되며, Counter/Gauge/Histogram 모두 포함됩니다.
 */
void append_prometheus_metrics(std::ostream& out);

/**
 * @brief core 런타임 핵심 카운터 스냅샷을 공통 메트릭 이름으로 출력합니다.
 *
 * 서비스별 구현 차이와 무관하게 최소 공통 관측 지표를 보장하기 위한 헬퍼입니다.
 */
void append_runtime_core_metrics(std::ostream& out);

/**
 * @brief 테스트용으로 공용 metrics backend 내부 상태를 초기화합니다.
 */
void reset_for_tests();

} // namespace server::core::metrics
