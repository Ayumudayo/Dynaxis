#include "client/client_app.hpp"
#include <iostream>

ClientApp::ClientApp() {
    setup_callbacks();
}

ClientApp::~ClientApp() {
    disconnect();
}

void ClientApp::update() {
    // 백그라운드 스레드에서 생성된 이벤트들을 메인 스레드에서 일괄 처리
    event_queue_.process_all();
}

bool ClientApp::connect(const std::string& host, int port) {
    if (net_client_.connect(host, static_cast<unsigned short>(port))) {
        // 즉시 true를 반환하지만, 실제 연결 성공 여부는 비동기임.
        // 하지만 여기서는 "연결 시도 시작 성공"의 의미로 사용.
        // 실제 UI 로그는 on_hello 등에서 찍힘.
        data_.is_connected = true; // 임시로 true, 연결 실패 시 콜백에서 false 처리
        return true;
    }
    return false;
}

void ClientApp::disconnect() {
    net_client_.close();
    data_.is_connected = false;
    data_.is_logged_in = false;
    data_.is_admin = false;
}

void ClientApp::login(const std::string& username) {
    net_client_.send_login(username, "");
}

/**
 * @brief 채팅 입력 처리 (핵심 비즈니스 로직)
 * 
 * 사용자가 입력한 문자열이 명령어인지 일반 채팅인지 판단하여
 * 적절한 네트워크 요청을 보냅니다.
 */
void ClientApp::process_command(const std::string& input) {
    if (input.empty()) return;

    if (input[0] == '/') {
        // 명령어 파싱 (/command arg1 arg2...)
        size_t split = input.find(' ');
        std::string cmd = (split == std::string::npos) ? input : input.substr(0, split);
        std::string args = (split == std::string::npos) ? "" : input.substr(split + 1);

        if (cmd == "/join") {
            if (!args.empty()) {
                size_t pass_split = args.find(' ');
                if (pass_split != std::string::npos) {
                    std::string room = args.substr(0, pass_split);
                    std::string pass = args.substr(pass_split + 1);
                    join_room(room, pass);
                } else {
                    join_room(args, "");
                }
            } else {
                data_.add_log("[usage] /join <room> [password]");
            }
        }
        else if (cmd == "/w" || cmd == "/whisper") {
            size_t msg_split = args.find(' ');
            if (msg_split != std::string::npos) {
                std::string target = args.substr(0, msg_split);
                std::string msg = args.substr(msg_split + 1);
                whisper(target, msg);
            } else {
                data_.add_log("[usage] /w <user> <message>");
            }
        }
        else {
            // 서버에서 처리하는 slash 명령(/invite, /kick, /mute, /ban 등)은 원문 그대로 전달한다.
            if (data_.is_connected && data_.is_logged_in) {
                net_client_.send_chat(data_.current_room, input);
            } else {
                data_.add_log("[error] Not connected or not logged in");
            }
        }
    } else {
        // 일반 채팅
        net_client_.send_chat(data_.current_room, input);
    }
}

void ClientApp::join_room(const std::string& room_name, const std::string& password) {
    net_client_.send_join(room_name, password);
}

void ClientApp::leave_current_room() {
    if (data_.current_room != "lobby") {
        net_client_.send_leave(data_.current_room);
    }
}

void ClientApp::whisper(const std::string& target, const std::string& message) {
    net_client_.send_whisper(target, message);
}

/**
 * @brief 네트워크 콜백 설정
 * 
 * 서버로부터 응답이 왔을 때 수행할 동작을 정의합니다.
 * 모든 콜백은 `event_queue_`를 통해 메인 스레드로 전달되어 실행되므로,
 * 데이터 경쟁(Data Race) 문제 없이 안전하게 UI 데이터를 수정할 수 있습니다.
 */
void ClientApp::setup_callbacks() {
    net_client_.set_on_hello([this](std::uint16_t caps) {
        event_queue_.push([this, caps]() {
            data_.add_log("[server] Hello received. Caps: " + std::to_string(caps));
        });
    });

    net_client_.set_on_err([this](std::uint16_t code, std::string msg) {
        event_queue_.push([this, code, msg]() {
            data_.add_log("[error] " + std::to_string(code) + ": " + msg);
        });
    });

    net_client_.set_on_login_res([this](std::string effective_user, std::uint32_t sid, bool is_admin) {
        event_queue_.push([this, effective_user, sid, is_admin]() {
            data_.is_logged_in = true;
            data_.is_admin = is_admin;
            data_.add_log("[system] Logged in as: " + effective_user + " (SID: " + std::to_string(sid) + ")");
        });
    });

    net_client_.set_on_disconnected([this](std::string reason) {
        event_queue_.push([this, reason]() {
            data_.is_connected = false;
            data_.is_logged_in = false;
            data_.is_admin = false;
            data_.add_log("[system] Disconnected: " + reason);
        });
    });

    net_client_.set_on_broadcast([this](std::string room, std::string sender, std::string text, std::uint16_t, std::uint32_t) {
         event_queue_.push([this, room, sender, text]() {
            // 채팅 포맷: [방이름] 보낸사람: 내용
            data_.add_log("[" + room + "] " + sender + ": " + text);
        });
    });

    net_client_.set_on_room_users([this](std::string room, std::vector<std::string> users) {
        event_queue_.push([this, room, users]() {
            // 현재 내가 보고 있는 방의 유저 목록인 경우에만 갱신
            if (room == data_.current_room) {
                data_.user_list = users;
                data_.add_log("[system] Room users updated (" + std::to_string(users.size()) + ")");
            }
        });
    });

    net_client_.set_on_snapshot([this](std::string cur_room, std::vector<std::string> rooms, std::vector<std::string> users, std::vector<bool> locked, std::vector<NetClient::SnapshotMessage> msgs, std::string) {
        event_queue_.push([this, cur_room, rooms, users, locked, msgs]() {
            data_.current_room = cur_room;
            data_.room_list.clear();
            for (size_t i = 0; i < rooms.size(); ++i) {
                bool is_locked = (i < locked.size()) ? locked[i] : false;
                data_.room_list.push_back({rooms[i], is_locked});
            }
            data_.user_list = users;
            
            data_.add_log("[system] Joined room: " + cur_room);
            for (const auto& m : msgs) {
                data_.add_log("[" + cur_room + "] " + m.sender + ": " + m.text);
            }
        });
    });

    net_client_.set_on_whisper([this](std::string sender, std::string recipient, std::string text, bool outgoing) {
        event_queue_.push([this, sender, recipient, text, outgoing]() {
            if (outgoing) data_.add_log("[whisper to " + recipient + "] " + text);
            else data_.add_log("[whisper from " + sender + "] " + text);
        });
    });
    
    net_client_.set_on_whisper_result([this](bool success, std::string reason) {
        event_queue_.push([this, success, reason]() {
            if (!success) data_.add_log("[error] Whisper failed: " + reason);
        });
    });

    // 동기화 이슈 해결을 위한 핵심 콜백
    net_client_.set_on_refresh_notify([this]() {
        event_queue_.push([this]() {
            if (data_.is_connected && data_.is_logged_in) {
                 net_client_.send_refresh(data_.current_room);
            }
        });
    });
}
