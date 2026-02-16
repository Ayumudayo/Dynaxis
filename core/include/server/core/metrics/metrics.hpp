#pragma once

#include <initializer_list>
#include <memory>
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
 * @return 구현체가 없으면 no-op 카운터를 반환
 */
Counter& get_counter(const std::string& name);

/**
 * @brief 이름으로 게이지를 조회합니다.
 * @param name 메트릭 이름
 * @return 구현체가 없으면 no-op 게이지를 반환
 */
Gauge& get_gauge(const std::string& name);

/**
 * @brief 이름으로 히스토그램을 조회합니다.
 * @param name 메트릭 이름
 * @return 구현체가 없으면 no-op 히스토그램을 반환
 */
Histogram& get_histogram(const std::string& name);

} // namespace server::core::metrics
