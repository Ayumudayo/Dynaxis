// Basic ImGui + GLFW + OpenGL3 main loop with NetClient Integration
#include "imgui.h"
#include "imgui_internal.h"
#include "misc/cpp/imgui_stdlib.h" // For InputText with std::string
#include "backends/imgui_impl_glfw.h"
#include "backends/imgui_impl_opengl3.h"
#include <GLFW/glfw3.h>
#include <cstdio>
#include <vector>
#include <string>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <chrono>

#include "client/net_client.hpp"

// -----------------------------------------------------------------------------
// Event Dispatcher for Thread Safety
// -----------------------------------------------------------------------------
struct EventQueue {
    std::deque<std::function<void()>> tasks;
    std::mutex mu;

    void push(std::function<void()> task) {
        std::lock_guard<std::mutex> lock(mu);
        tasks.push_back(std::move(task));
    }

    void process() {
        std::vector<std::function<void()>> current_batch;
        {
            std::lock_guard<std::mutex> lock(mu);
            if (tasks.empty()) return;
            while (!tasks.empty()) {
                current_batch.push_back(std::move(tasks.front()));
                tasks.pop_front();
            }
        }
        for (auto& task : current_batch) {
            task();
        }
    }
} g_event_queue;

// -----------------------------------------------------------------------------
// Global Application State
// -----------------------------------------------------------------------------
struct RoomInfo {
    std::string name;
    bool locked;
};

struct AppState {
    char host_buffer[128] = "127.0.0.1";
    int port = 36000;
    char username_buffer[128] = "guest";
    
    bool is_connected = false;
    bool is_logged_in = false;
    
    std::string current_room = "lobby";
    std::vector<std::string> user_list;
    std::vector<RoomInfo> room_list;

    std::vector<std::string> logs;
    bool auto_scroll = true;
    
    char chat_input[256] = "";
    bool focus_chat_input = false;

    // State for Join Password Modal
    bool show_password_modal = false;
    std::string target_room_to_join;
    char password_buffer[64] = "";

    void add_log(const std::string& msg) {
        // Must be called from main thread or via EventQueue
        logs.push_back(msg);
    }
} g_app_state;

// -----------------------------------------------------------------------------
// NetClient Instance
// -----------------------------------------------------------------------------
NetClient g_net_client;

void ProcessCommand(std::string input) {
    if (input.empty()) return;
    if (input[0] == '/') {
        // Command parsing
        size_t split = input.find(' ');
        std::string cmd = (split == std::string::npos) ? input : input.substr(0, split);
        std::string args = (split == std::string::npos) ? "" : input.substr(split + 1);

        if (cmd == "/join") {
            if (!args.empty()) {
                size_t pass_split = args.find(' ');
                if (pass_split != std::string::npos) {
                     g_net_client.send_join(args.substr(0, pass_split), args.substr(pass_split+1));
                } else {
                     g_net_client.send_join(args, "");
                }
            }
            else g_app_state.add_log("[usage] /join <room> [password]");
        }
        else if (cmd == "/w") {
            size_t msg_split = args.find(' ');
            if (msg_split != std::string::npos) {
                std::string target = args.substr(0, msg_split);
                std::string msg = args.substr(msg_split + 1);
                g_net_client.send_whisper(target, msg);
            } else {
                g_app_state.add_log("[usage] /w <user> <message>");
            }
        }
        else {
             g_app_state.add_log("[error] Unknown command: " + cmd);
        }
    } else {
        // Normal chat
        g_net_client.send_chat(g_app_state.current_room, input);
    }
}

// -----------------------------------------------------------------------------
// Callbacks
// -----------------------------------------------------------------------------
void SetupCallbacks() {
    g_net_client.set_on_hello([](std::uint16_t caps) {
        g_event_queue.push([caps]() {
            g_app_state.add_log("[server] Hello received. Caps: " + std::to_string(caps));
        });
    });

    g_net_client.set_on_err([](std::uint16_t code, std::string msg) {
        g_event_queue.push([code, msg]() {
            g_app_state.add_log("[error] " + std::to_string(code) + ": " + msg);
        });
    });

    g_net_client.set_on_login_res([](std::string effective_user, std::uint32_t sid) {
        g_event_queue.push([effective_user, sid]() {
            g_app_state.is_logged_in = true;
            g_app_state.add_log("[system] Logged in as: " + effective_user + " (SID: " + std::to_string(sid) + ")");
        });
    });

    g_net_client.set_on_disconnected([](std::string reason) {
        g_event_queue.push([reason]() {
            g_app_state.is_connected = false;
            g_app_state.is_logged_in = false;
            g_app_state.add_log("[system] Disconnected: " + reason);
        });
    });

    g_net_client.set_on_broadcast([](std::string room, std::string sender, std::string text, std::uint16_t flags, std::uint32_t) {
         g_event_queue.push([room, sender, text]() {
            g_app_state.add_log("[" + room + "] " + sender + ": " + text);
        });
    });

    g_net_client.set_on_room_users([](std::string room, std::vector<std::string> users) {
        g_event_queue.push([room, users]() {
            if (room == g_app_state.current_room) {
                g_app_state.user_list = users;
                g_app_state.add_log("[system] Room users updated (" + std::to_string(users.size()) + ")");
            }
        });
    });

    g_net_client.set_on_snapshot([](std::string cur_room, std::vector<std::string> rooms, std::vector<std::string> users, std::vector<bool> locked, std::vector<NetClient::SnapshotMessage> msgs, std::string my_name) {
        g_event_queue.push([cur_room, rooms, users, locked, msgs, my_name]() {
            g_app_state.current_room = cur_room;
            g_app_state.room_list.clear();
            for (size_t i = 0; i < rooms.size(); ++i) {
                bool is_locked = (i < locked.size()) ? locked[i] : false;
                g_app_state.room_list.push_back({rooms[i], is_locked});
            }
            g_app_state.user_list = users;
            // g_app_state.username_buffer = my_name; 
            g_app_state.add_log("[system] Joined room: " + cur_room);
            for (const auto& m : msgs) {
                g_app_state.add_log("[" + cur_room + "] " + m.sender + ": " + m.text);
            }
        });
    });

    g_net_client.set_on_whisper([](std::string sender, std::string recipient, std::string text, bool outgoing) {
        g_event_queue.push([sender, recipient, text, outgoing]() {
            if (outgoing) g_app_state.add_log("[whisper to " + recipient + "] " + text);
            else g_app_state.add_log("[whisper from " + sender + "] " + text);
        });
    });
    
    g_net_client.set_on_whisper_result([](bool success, std::string reason) {
        g_event_queue.push([success, reason]() {
            if (!success) g_app_state.add_log("[error] Whisper failed: " + reason);
        });
    });

    g_net_client.set_on_refresh_notify([]() {
        g_event_queue.push([]() {
            if (g_app_state.is_connected && g_app_state.is_logged_in) {
                 g_net_client.send_refresh(g_app_state.current_room);
            }
        });
    });
}

static void glfw_error_callback(int error, const char* description)
{
    fprintf(stderr, "GLFW Error %d: %s\n", error, description);
}

int main(int, char**)
{
    glfwSetErrorCallback(glfw_error_callback);
    if (!glfwInit())
        return 1;

    const char* glsl_version = "#version 130";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);

    GLFWwindow* window = glfwCreateWindow(1280, 720, "Knights ImGui Client", NULL, NULL);
    if (window == NULL)
        return 1;
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;     
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;      

    ImGui::StyleColorsDark();

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init(glsl_version);

    SetupCallbacks();

    // Fonts
    ImFontConfig config;
    config.MergeMode = false;
    const char* font_path = "C:\\Windows\\Fonts\\malgun.ttf";
    FILE* f = fopen(font_path, "rb");
    if (f) {
        fclose(f);
        io.Fonts->AddFontFromFileTTF(font_path, 18.0f, NULL, io.Fonts->GetGlyphRangesKorean());
    } else {
        io.Fonts->AddFontDefault();
    }

    ImVec4 clear_color = ImVec4(0.45f, 0.55f, 0.60f, 1.00f);
    bool show_demo_window = false;

    while (!glfwWindowShouldClose(window))
    {
        glfwPollEvents();
        g_event_queue.process();

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        // ---------------------------------------------------------------------
        // DockSpace Setup
        // ---------------------------------------------------------------------
        ImGuiWindowFlags window_flags = ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoDocking;
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->WorkPos);
        ImGui::SetNextWindowSize(viewport->WorkSize);
        ImGui::SetNextWindowViewport(viewport->ID);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        window_flags |= ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove;
        window_flags |= ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        
        ImGui::Begin("DockSpace Demo", nullptr, window_flags);
        ImGui::PopStyleVar(3);

        ImGuiID dockspace_id = ImGui::GetID("MyDockSpace");

        // Fixed Layout Initialization
        // Try to initialize layout until successful (requires valid viewport size)
        static bool layout_initialized = false;
        if (!layout_initialized || ImGui::DockBuilderGetNode(dockspace_id) == NULL)
        {
            if (viewport->WorkSize.x > 0 && viewport->WorkSize.y > 0)
            {
                ImGui::DockBuilderRemoveNode(dockspace_id); 
                ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace | ImGuiDockNodeFlags_NoWindowMenuButton);
                ImGui::DockBuilderSetNodeSize(dockspace_id, viewport->WorkSize);

                ImGuiID dock_main_id = dockspace_id;
                ImGuiID dock_id_left = ImGui::DockBuilderSplitNode(dock_main_id, ImGuiDir_Left, 0.25f, NULL, &dock_main_id);
                ImGuiID dock_id_left_bottom = ImGui::DockBuilderSplitNode(dock_id_left, ImGuiDir_Down, 0.50f, NULL, &dock_id_left);
                
                ImGui::DockBuilderDockWindow("Rooms", dock_id_left);
                ImGui::DockBuilderDockWindow("Users", dock_id_left_bottom);
                ImGui::DockBuilderDockWindow("Chat", dock_main_id);

                ImGui::DockBuilderFinish(dockspace_id);
                layout_initialized = true;
            }
        }

        ImGui::DockSpace(dockspace_id, ImVec2(0.0f, 0.0f), ImGuiDockNodeFlags_None);

        if (ImGui::BeginMenuBar()) {
            if (ImGui::BeginMenu("File")) {
                if (ImGui::MenuItem("Exit")) glfwSetWindowShouldClose(window, true);
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("View")) {
                if (ImGui::MenuItem("Reset Layout")) { layout_initialized = false; ImGui::DockBuilderRemoveNode(dockspace_id); } 
                ImGui::MenuItem("ImGui Demo", NULL, &show_demo_window);
                ImGui::EndMenu();
            }
            ImGui::EndMenuBar();
        }
        ImGui::End();

        if (show_demo_window) ImGui::ShowDemoWindow(&show_demo_window);

        // UI Logic
        if (!g_app_state.is_connected) {
            ImGui::OpenPopup("Login");
        }
        
        // Center Login
        ImVec2 center = ImGui::GetMainViewport()->GetCenter();
        ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

        if (ImGui::BeginPopupModal("Login", NULL, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove)) {
            ImGui::Text("Connect to Knights Server");
            ImGui::Separator();
            ImGui::InputText("Host", g_app_state.host_buffer, sizeof(g_app_state.host_buffer));
            ImGui::InputInt("Port", &g_app_state.port);
            ImGui::InputText("Username", g_app_state.username_buffer, sizeof(g_app_state.username_buffer));
            ImGui::Separator();

            if (ImGui::Button("Connect", ImVec2(120, 0))) {
                if (g_net_client.connect(g_app_state.host_buffer, static_cast<unsigned short>(g_app_state.port))) {
                    ImGui::CloseCurrentPopup();
                    g_app_state.is_connected = true;
                    g_app_state.add_log("[system] Connected to server.");
                    g_net_client.send_login(g_app_state.username_buffer, ""); 
                } else {
                    g_app_state.add_log("[error] Failed to connect.");
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(120, 0))) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }

        // Rooms Sidebar
        ImGui::Begin("Rooms", NULL, ImGuiWindowFlags_NoCollapse); // Fixed? Docking controls flags mostly
        if (g_app_state.is_connected) {
             ImGui::Text("Current: %s", g_app_state.current_room.c_str());

             if (g_app_state.current_room != "lobby") {
                 ImGui::SameLine();
                 if (ImGui::Button("Leave")) {
                     g_net_client.send_leave(g_app_state.current_room);
                 }
             }

             // Make Room Button & Modal
             if (ImGui::Button("Make Room", ImVec2(-1, 0))) {
                 ImGui::OpenPopup("Create Room");
                 g_app_state.target_room_to_join = ""; // Reset for name input
                 g_app_state.password_buffer[0] = '\0';
             }
             
             // Create Room Modal
             if (ImGui::BeginPopupModal("Create Room", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
                 static char new_room_name[64] = "";
                 ImGui::InputText("Room Name", new_room_name, sizeof(new_room_name));
                 ImGui::InputText("Password (Optional)", g_app_state.password_buffer, sizeof(g_app_state.password_buffer), ImGuiInputTextFlags_Password);
                 
                 if (ImGui::Button("Create", ImVec2(120, 0))) {
                     if (strlen(new_room_name) > 0) {
                         g_net_client.send_join(new_room_name, g_app_state.password_buffer);
                         ImGui::CloseCurrentPopup();
                         new_room_name[0] = '\0'; // Reset
                     }
                 }
                 ImGui::SameLine();
                 if (ImGui::Button("Cancel", ImVec2(120, 0))) {
                     ImGui::CloseCurrentPopup();
                 }
                 ImGui::EndPopup();
             }

             ImGui::Separator();
             for (const auto& r : g_app_state.room_list) {
                 std::string label = r.name + (r.locked ? " [L]" : "");
                 if (ImGui::Selectable(label.c_str(), r.name == g_app_state.current_room)) {
                     if (r.locked) {
                         g_app_state.target_room_to_join = r.name;
                         g_app_state.show_password_modal = true;
                     } else {
                         g_net_client.send_join(r.name, "");
                     }
                 }
                 if (ImGui::BeginPopupContextItem()) {
                     if (ImGui::MenuItem("Join")) {
                         if (r.locked) {
                             g_app_state.target_room_to_join = r.name;
                             g_app_state.show_password_modal = true;
                         } else {
                             g_net_client.send_join(r.name, "");
                         }
                     }
                     ImGui::EndPopup();
                 }
             }
             if (g_app_state.room_list.empty()) ImGui::TextDisabled("No rooms listed");
             
             ImGui::Separator();
             static char join_buf[64] = "";
             ImGui::InputText("Join...", join_buf, sizeof(join_buf));
             ImGui::SameLine();
             if (ImGui::Button("Go") && join_buf[0] != 0) {
                 g_net_client.send_join(join_buf, "");
                 join_buf[0] = 0;
             }
        } else {
             ImGui::Text("Not connected.");
        }
        ImGui::End();

        // Password Modal
        if (g_app_state.show_password_modal) {
            ImGui::OpenPopup("Join Locked Room");
            g_app_state.show_password_modal = false;
            g_app_state.password_buffer[0] = '\0';
        }
        if (ImGui::BeginPopupModal("Join Locked Room", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("Enter password for: %s", g_app_state.target_room_to_join.c_str());
            if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere(0);
            ImGui::InputText("Password", g_app_state.password_buffer, sizeof(g_app_state.password_buffer), ImGuiInputTextFlags_Password | ImGuiInputTextFlags_EnterReturnsTrue);
            
            if (ImGui::Button("Join", ImVec2(120, 0)) || ImGui::IsKeyPressed(ImGuiKey_Enter)) {
                g_net_client.send_join(g_app_state.target_room_to_join, g_app_state.password_buffer);
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(120, 0))) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }

        // Users Sidebar
        ImGui::Begin("Users", NULL, ImGuiWindowFlags_NoCollapse);
        if (g_app_state.is_connected) {
            ImGui::Text("Online Users (%d)", (int)g_app_state.user_list.size());
            ImGui::Separator();
            for (const auto& u : g_app_state.user_list) {
                ImGui::Selectable(u.c_str());
                if (ImGui::BeginPopupContextItem()) {
                    if (ImGui::MenuItem("Whisper")) {
                        snprintf(g_app_state.chat_input, sizeof(g_app_state.chat_input), "/w %s ", u.c_str());
                        g_app_state.focus_chat_input = true;
                    }
                    if (ImGui::MenuItem("Block")) {
                        g_app_state.add_log("[system] Blocked user: " + u);
                    }
                    ImGui::EndPopup();
                }
            }
        }
        ImGui::End();

        // Chat
        ImGui::Begin("Chat");
        {
            ImGui::BeginChild("ScrollingRegion", ImVec2(0, -ImGui::GetFrameHeightWithSpacing()), false, ImGuiWindowFlags_HorizontalScrollbar);
            for (const auto& item : g_app_state.logs) ImGui::TextUnformatted(item.c_str());
            if (g_app_state.auto_scroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) ImGui::SetScrollHereY(1.0f);
            ImGui::EndChild();
            
            ImGui::Separator();
            ImGui::PushItemWidth(-1);
            if (g_app_state.focus_chat_input) {
                ImGui::SetKeyboardFocusHere();
                g_app_state.focus_chat_input = false;
            }
            if (ImGui::InputText("##Input", g_app_state.chat_input, sizeof(g_app_state.chat_input), ImGuiInputTextFlags_EnterReturnsTrue)) {
                if (g_app_state.is_connected && g_app_state.is_logged_in) {
                     ProcessCommand(g_app_state.chat_input);
                     g_app_state.chat_input[0] = '\0';
                     ImGui::SetKeyboardFocusHere(-1); 
                } else {
                    g_app_state.add_log("[warn] Not logged in!");
                    ImGui::SetKeyboardFocusHere(-1);
                }
            }
            ImGui::PopItemWidth();
        }
        ImGui::End();

        ImGui::Render();
        int display_w, display_h;
        glfwGetFramebufferSize(window, &display_w, &display_h);
        glViewport(0, 0, display_w, display_h);
        glClearColor(clear_color.x * clear_color.w, clear_color.y * clear_color.w, clear_color.z * clear_color.w, clear_color.w);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        glfwSwapBuffers(window);
    }
    g_net_client.close();

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    glfwDestroyWindow(window);
    glfwTerminate();

    return 0;
}
