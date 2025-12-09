#pragma once

#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <vector>
#include <memory>
#include <atomic>

#include "client/net_client.hpp"

/**
 * @brief 방 정보 구조체
 * 
 * 서버에서 받은 방 목록의 개별 항목을 저장합니다.
 */
struct RoomInfo {
    std::string name; ///< 방 이름
    bool locked;      ///< 비밀번호 잠금 여부
};

/**
 * @brief 애플리케이션의 핵심 데이터 상태 (Data State)
 * 
 * UI와 무관하게, 애플리케이션이 유지해야 할 핵심 데이터입니다.
 */
struct AppData {
    bool is_connected = false;
    bool is_logged_in = false;
    
    std::string current_room = "lobby";
    std::vector<std::string> user_list;
    std::vector<RoomInfo> room_list;
    
    // 채팅 로그 (채팅창에 표시될 내용)
    std::vector<std::string> logs;

    // 로그 추가 헬퍼
    void add_log(const std::string& msg) {
        logs.push_back(msg);
    }
    
    // 마지막 로그 가져오기 (디버그용)
    std::string get_last_log() const {
        if (logs.empty()) return "";
        return logs.back();
    }
};

/**
 * @brief 이벤트 큐 (Thread-Safe Event Queue)
 * 
 * 네트워크 스레드(백그라운드)에서 발생한 이벤트를
 * 메인 스레드(UI 스레드)로 안전하게 전달하기 위해 사용합니다.
 */
class EventQueue {
public:
    void push(std::function<void()> task) {
        std::lock_guard<std::mutex> lock(mu_);
        tasks_.push_back(std::move(task));
    }

    void process_all() {
        std::vector<std::function<void()>> current_batch;
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (tasks_.empty()) return;
            // 큐의 내용을 모두 가져와서 배치로 처리 (Lock 시간 최소화)
            while (!tasks_.empty()) {
                current_batch.push_back(std::move(tasks_.front()));
                tasks_.pop_front();
            }
        }
        for (auto& task : current_batch) {
            task();
        }
    }

private:
    std::deque<std::function<void()>> tasks_;
    std::mutex mu_;
};

/**
 * @brief 클라이언트 애플리케이션 클래스 (Facade & Controller)
 * 
 * - **역할 (Role)**: 
 *   1. 네트워크 클라이언트(`NetClient`)를 소유하고 관리합니다.
 *   2. 애플리케이션의 데이터(`AppData`)를 관리합니다.
 *   3. 사용자 명령(채팅, 명령어 등)을 처리하여 네트워크 요청으로 변환합니다.
 *   4. 서버로부터 온 이벤트를 처리하여 데이터를 갱신합니다. (Model update)
 * 
 * - **SOLID 원칙 적용**:
 *   - **SRP (단일 책임 원칙)**: UI 렌더링 코드를 완전히 배제하고, "로직"과 "데이터"에만 집중합니다.
 */
class ClientApp {
public:
    ClientApp();
    ~ClientApp();

    // --- 메인 루프에서 호출해야 하는 메서드 ---

    /**
     * @brief 틱(Tick) 업데이트
     * 
     * 매 프레임마다 호출되어 이벤트 큐에 쌓인 작업을 처리합니다.
     */
    void update();

    // --- 사용자 액션 (User Actions) ---

    // 연결 시도
    bool connect(const std::string& host, int port);
    
    // 연결 종료
    void disconnect();

    // 로그인
    void login(const std::string& username);

    // 채팅 메시지 또는 명령어 처리
    // 예: "/join room" 또는 "hello world"
    void process_command(const std::string& input);

    // 방 만들기/입장
    void join_room(const std::string& room_name, const std::string& password = "");

    // 현재 방 나가기 (로비로 이동)
    void leave_current_room();

    // 귓속말
    void whisper(const std::string& target, const std::string& message);

    // --- 상태 접근 (Getters) ---
    
    const AppData& get_data() const { return data_; }
    // UI에서만 사용하는 임시 상태(버퍼 등)가 아니라, 핵심 데이터는 읽기 전용으로 제공하는 것이 안전합니다.
    // 하지만 ImGui 특성상 편의를 위해 필요시 수정 기능을 제공할 수도 있습니다. 현재는 읽기 전용.

private:
    // 네트워크 콜백 설정
    void setup_callbacks();

private:
    NetClient net_client_;
    AppData data_;
    EventQueue event_queue_;
};
