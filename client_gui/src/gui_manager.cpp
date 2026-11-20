#include "client/gui_manager.hpp"

#include "imgui.h"
#include "imgui_internal.h"
#include "misc/cpp/imgui_stdlib.h"
#include "backends/imgui_impl_glfw.h"
#include "backends/imgui_impl_opengl3.h"
#include <GLFW/glfw3.h>

#if defined(_WIN32)
#undef APIENTRY
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef GLFW_EXPOSE_NATIVE_WIN32
#define GLFW_EXPOSE_NATIVE_WIN32
#endif
#include <GLFW/glfw3native.h>
#include <imm.h>
#include <shlobj.h>
#ifdef ERROR
#undef ERROR
#endif
#endif

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

/**
 * @brief ImGui/GLFW 기반 메신저·워크벤치 UI 렌더링 및 입력 처리 구현입니다.
 *
 * 단일 상태 모델 위에서 기본 `MESSENGER` 뷰와 `WORKBENCH` 진단 서랍을 전환하고,
 * DPI 인식 선명한 폰트 래스터화, Windows 버퍼 직접 조합(box-less) 한글/CJK IME 브리지, 여유 있는 여백 계층을 제공합니다.
 */

namespace {

constexpr float kBaseFontSizePx = 19.0f;
constexpr float kMinTextZoom = 0.85f;
constexpr float kMaxTextZoom = 1.40f;

void glfw_error_callback(int error, const char* description) {
    std::fprintf(stderr, "GLFW Error %d: %s\n", error, description);
}

bool contains_case_insensitive(const std::string& haystack, const char* needle) {
    if (!needle || needle[0] == '\0') {
        return true;
    }
    const std::string_view needle_sv(needle);
    return std::search(
               haystack.begin(),
               haystack.end(),
               needle_sv.begin(),
               needle_sv.end(),
               [](unsigned char a, unsigned char b) {
                   return std::tolower(a) == std::tolower(b);
               }) != haystack.end();
}

std::string format_timestamp_hms(std::uint64_t ts_ms) {
    if (ts_ms == 0) {
        return "--:--:--";
    }
    const std::time_t seconds = static_cast<std::time_t>(ts_ms / 1000ULL);
    std::tm tm_buf{};
#if defined(_WIN32)
    if (localtime_s(&tm_buf, &seconds) != 0) {
        return "--:--:--";
    }
#else
    if (localtime_r(&seconds, &tm_buf) == nullptr) {
        return "--:--:--";
    }
#endif
    char buffer[16]{};
    if (std::strftime(buffer, sizeof(buffer), "%H:%M:%S", &tm_buf) == 0) {
        return "--:--:--";
    }
    return std::string(buffer);
}

std::string format_with_commas(std::size_t value) {
    const std::string raw = std::to_string(value);
    if (raw.size() <= 3) {
        return raw;
    }
    std::string out;
    out.reserve(raw.size() + raw.size() / 3);
    const std::size_t first_group = (raw.size() % 3 == 0) ? 3 : (raw.size() % 3);
    out.append(raw, 0, first_group);
    for (std::size_t i = first_group; i < raw.size(); i += 3) {
        out.push_back(',');
        out.append(raw, i, 3);
    }
    return out;
}

std::uint64_t current_unix_ms_wall() {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
    return ms < 0 ? 0ULL : static_cast<std::uint64_t>(ms);
}

const char* protocol_direction_label(ProtocolDirection dir) {
    switch (dir) {
        case ProtocolDirection::TX: return "TX ";
        case ProtocolDirection::RX: return "RX ";
        case ProtocolDirection::LIFECYCLE: return "NET";
    }
    return "NET";
}

ImVec4 protocol_direction_color(ProtocolDirection dir) {
    switch (dir) {
        case ProtocolDirection::TX: return ImVec4(0.46f, 0.86f, 0.84f, 1.0f);
        case ProtocolDirection::RX: return ImVec4(0.42f, 0.76f, 0.98f, 1.0f);
        case ProtocolDirection::LIFECYCLE: return ImVec4(0.96f, 0.72f, 0.30f, 1.0f);
    }
    return ImVec4(0.75f, 0.78f, 0.85f, 1.0f);
}

const char* transport_state_label(TransportState state) {
    switch (state) {
        case TransportState::CONNECTED: return "Connected";
        case TransportState::CONNECTING: return "Connecting";
        case TransportState::DISCONNECTED: return "Disconnected";
    }
    return "Disconnected";
}

ImVec4 transport_state_color(TransportState state) {
    switch (state) {
        case TransportState::CONNECTED: return ImVec4(0.24f, 0.78f, 0.52f, 1.0f);
        case TransportState::CONNECTING: return ImVec4(0.92f, 0.72f, 0.24f, 1.0f);
        case TransportState::DISCONNECTED: return ImVec4(0.84f, 0.36f, 0.38f, 1.0f);
    }
    return ImVec4(0.84f, 0.36f, 0.38f, 1.0f);
}

struct SlashSuggestion {
    const char* command;
    const char* syntax;
    const char* description;
    bool admin_only;
};

constexpr std::array<SlashSuggestion, 17> kSlashSuggestions{{
    {"/join ", "/join <room> [password]", "Join or create a room", false},
    {"/w ", "/w <user> <message>", "Send a private whisper", false},
    {"/leave", "/leave", "Return to lobby", false},
    {"/refresh", "/refresh", "Refresh room snapshot", false},
    {"/rooms", "/rooms", "Request active room list", false},
    {"/who ", "/who [room]", "List room participants", false},
    {"/invite ", "/invite <user> [room]", "Invite user to room", false},
    {"/kick ", "/kick <user> [room]", "Kick user from room", false},
    {"/room remove ", "/room remove [room]", "Close and remove room", false},
    {"/block ", "/block <user>", "Block private messages from user", false},
    {"/unblock ", "/unblock <user>", "Remove user from block list", false},
    {"/blacklist list", "/blacklist list", "Sync personal block list", false},
    {"/mute ", "/mute <user> [sec]", "Mute user globally (admin)", true},
    {"/unmute ", "/unmute <user>", "Remove global mute (admin)", true},
    {"/ban ", "/ban <user> [sec]", "Ban user globally (admin)", true},
    {"/unban ", "/unban <user>", "Remove global ban (admin)", true},
    {"/gkick ", "/gkick <user>", "Force disconnect user (admin)", true},
}};

void push_accent_button_style() {
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.21f, 0.42f, 0.90f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.26f, 0.48f, 0.96f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.18f, 0.36f, 0.80f, 1.0f));
}

void pop_accent_button_style() {
    ImGui::PopStyleColor(3);
}

float calc_button_width(const char* label, float extra_width = 0.0f) {
    const ImGuiStyle& style = ImGui::GetStyle();
    return std::ceil(ImGui::CalcTextSize(label).x + style.FramePadding.x * 2.0f + extra_width);
}

float selectable_row_height() {
    const ImGuiStyle& style = ImGui::GetStyle();
    return std::round(ImGui::GetTextLineHeight() + style.FramePadding.y * 1.15f);
}

ImFontConfig make_crisp_font_config(bool merge_mode) {
    ImFontConfig cfg;
    cfg.MergeMode = merge_mode;
    cfg.PixelSnapH = true;
    cfg.OversampleH = 1;
    cfg.OversampleV = 1;
    cfg.RasterizerMultiply = 1.15f;
    return cfg;
}

#if defined(_WIN32)
constexpr const wchar_t* kImeHookPropName = L"DynaxisImeHookState";

struct Win32ImeHookState {
    WNDPROC prev_wndproc{nullptr};
    ImGuiPlatformImeData ime_data{};
    std::string comp_utf8;
    bool composing{false};
    bool candidate_open{false};
    bool comp_dirty{false};
    std::uint64_t commit_count{0};
    ImGuiID preedit_id{0};
    int preedit_pos{-1};
    int preedit_len{0};
    std::string applied_preedit;
};

Win32ImeHookState* get_ime_hook_state(HWND hwnd) {
    if (!hwnd) {
        return nullptr;
    }
    return static_cast<Win32ImeHookState*>(::GetPropW(hwnd, kImeHookPropName));
}

Win32ImeHookState* get_ime_hook_state(GLFWwindow* window) {
    if (!window) {
        return nullptr;
    }
    return get_ime_hook_state(glfwGetWin32Window(window));
}

bool has_queued_text_events(const ImGuiContext& ctx) {
    for (int i = 0; i < ctx.InputEventsQueue.Size; ++i) {
        if (ctx.InputEventsQueue[i].Type == ImGuiInputEventType_Text) {
            return true;
        }
    }
    return false;
}

std::string wide_to_utf8(std::wstring_view wide) {
    if (wide.empty()) {
        return {};
    }
    const int needed = ::WideCharToMultiByte(
        CP_UTF8,
        0,
        wide.data(),
        static_cast<int>(wide.size()),
        nullptr,
        0,
        nullptr,
        nullptr);
    if (needed <= 0) {
        return {};
    }
    std::string out(static_cast<std::size_t>(needed), '\0');
    const int written = ::WideCharToMultiByte(
        CP_UTF8,
        0,
        wide.data(),
        static_cast<int>(wide.size()),
        out.data(),
        needed,
        nullptr,
        nullptr);
    if (written <= 0) {
        return {};
    }
    return out;
}

std::wstring read_imm_comp_string(HIMC himc, DWORD index) {
    const LONG bytes = ::ImmGetCompositionStringW(himc, index, nullptr, 0);
    if (bytes <= 0) {
        return {};
    }
    std::wstring buf(static_cast<std::size_t>(bytes) / sizeof(wchar_t), L'\0');
    const LONG copied = ::ImmGetCompositionStringW(himc, index, buf.data(), static_cast<DWORD>(bytes));
    if (copied <= 0) {
        return {};
    }
    buf.resize(static_cast<std::size_t>(copied) / sizeof(wchar_t));
    return buf;
}

void feed_utf16_to_imgui(ImGuiIO& io, const std::wstring& wide) {
    for (wchar_t ch : wide) {
        if (ch != L'\0') {
            io.AddInputCharacterUTF16(static_cast<ImWchar16>(ch));
        }
    }
}

void dynaxis_set_ime_data(ImGuiContext*, ImGuiViewport* viewport, ImGuiPlatformImeData* data) {
    if (!viewport || !data) {
        return;
    }
    HWND hwnd = static_cast<HWND>(viewport->PlatformHandleRaw);
    if (!hwnd) {
        return;
    }
    if (Win32ImeHookState* state = get_ime_hook_state(hwnd)) {
        state->ime_data = *data;
        state->ime_data.ViewportId = viewport->ID;
    }

    if (HIMC himc = ::ImmGetContext(hwnd)) {
        const LONG local_x = static_cast<LONG>(std::round(data->InputPos.x - viewport->Pos.x));
        const LONG local_y = static_cast<LONG>(std::round(data->InputPos.y - viewport->Pos.y + data->InputLineHeight + 2.0f));

        COMPOSITIONFORM composition_form{};
        composition_form.dwStyle = CFS_FORCE_POSITION;
        composition_form.ptCurrentPos.x = local_x;
        composition_form.ptCurrentPos.y = local_y;
        ::ImmSetCompositionWindow(himc, &composition_form);

        CANDIDATEFORM candidate_form{};
        candidate_form.dwIndex = 0;
        candidate_form.dwStyle = CFS_CANDIDATEPOS;
        candidate_form.ptCurrentPos.x = local_x;
        candidate_form.ptCurrentPos.y = local_y;
        ::ImmSetCandidateWindow(himc, &candidate_form);

        ::ImmReleaseContext(hwnd, himc);
    }
}

LRESULT CALLBACK dynaxis_ime_wndproc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    Win32ImeHookState* state = get_ime_hook_state(hwnd);
    if (!state) {
        return ::DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    WNDPROC prev_wndproc = state->prev_wndproc;

    switch (msg) {
        case WM_IME_SETCONTEXT:
            // Suppress Windows default GDI composition popup; preedit updates directly inside InputText buffer.
            lParam &= ~static_cast<LPARAM>(ISC_SHOWUICOMPOSITIONWINDOW);
            return ::CallWindowProcW(prev_wndproc, hwnd, msg, wParam, lParam);

        case WM_IME_NOTIFY:
            if (wParam == IMN_OPENCANDIDATE) {
                state->candidate_open = true;
            } else if (wParam == IMN_CLOSECANDIDATE) {
                state->candidate_open = false;
            }
            return ::CallWindowProcW(prev_wndproc, hwnd, msg, wParam, lParam);

        case WM_IME_STARTCOMPOSITION:
            state->composing = true;
            state->comp_utf8.clear();
            state->comp_dirty = true;
            return 0;

        case WM_IME_COMPOSITION: {
            if (lParam == 0) {
                state->comp_utf8.clear();
                state->composing = false;
                state->comp_dirty = true;
                return 0;
            }
            if (HIMC himc = ::ImmGetContext(hwnd)) {
                if (lParam & GCS_RESULTSTR) {
                    const std::wstring committed = read_imm_comp_string(himc, GCS_RESULTSTR);
                    if (!committed.empty() && ImGui::GetCurrentContext() != nullptr) {
                        feed_utf16_to_imgui(ImGui::GetIO(), committed);
                        ++state->commit_count;
                    }
                    state->comp_utf8.clear();
                    state->comp_dirty = true;
                }
                if (lParam & GCS_COMPSTR) {
                    const std::wstring comp = read_imm_comp_string(himc, GCS_COMPSTR);
                    state->comp_utf8 = wide_to_utf8(comp);
                    state->composing = !state->comp_utf8.empty();
                    state->comp_dirty = true;
                } else if (lParam & GCS_RESULTSTR) {
                    state->composing = false;
                }
                ::ImmReleaseContext(hwnd, himc);
            }
            return 0;
        }

        case WM_IME_ENDCOMPOSITION:
            state->composing = false;
            state->comp_utf8.clear();
            state->candidate_open = false;
            state->comp_dirty = true;
            return 0;

        case WM_KILLFOCUS:
            state->composing = false;
            state->comp_utf8.clear();
            state->candidate_open = false;
            state->comp_dirty = true;
            break;

        case WM_KEYDOWN:
            if (wParam == VK_PROCESSKEY && ImGui::GetCurrentContext() != nullptr) {
                const UINT real_vk = ::ImmGetVirtualKey(hwnd);
                const HKL layout = ::GetKeyboardLayout(0);
                const WORD primary_lang = PRIMARYLANGID(LOWORD(reinterpret_cast<std::uintptr_t>(layout)));
                ImGuiIO& io = ImGui::GetIO();
                if (real_vk == VK_RETURN && primary_lang == LANG_KOREAN && !state->candidate_open && io.WantTextInput) {
                    if (HIMC himc = ::ImmGetContext(hwnd)) {
                        if (::ImmGetCompositionStringW(himc, GCS_COMPSTR, nullptr, 0) > 0) {
                            const std::uint64_t before_commits = state->commit_count;
                            const std::wstring fallback_comp = read_imm_comp_string(himc, GCS_COMPSTR);
                            ::ImmNotifyIME(himc, NI_COMPOSITIONSTR, CPS_COMPLETE, 0);
                            if (state->commit_count == before_commits && !fallback_comp.empty()) {
                                ::ImmNotifyIME(himc, NI_COMPOSITIONSTR, CPS_CANCEL, 0);
                                feed_utf16_to_imgui(io, fallback_comp);
                                ++state->commit_count;
                            }
                        }
                        ::ImmReleaseContext(hwnd, himc);
                    }
                    state->comp_utf8.clear();
                    state->composing = false;
                    state->comp_dirty = true;
                    io.AddKeyEvent(ImGuiKey_Enter, true);
                    io.AddKeyEvent(ImGuiKey_Enter, false);
                    return 0;
                }
            }
            break;

        default:
            break;
    }

    return ::CallWindowProcW(prev_wndproc, hwnd, msg, wParam, lParam);
}

void install_win32_ime_hook(GLFWwindow* window) {
    HWND hwnd = glfwGetWin32Window(window);
    if (!hwnd || get_ime_hook_state(hwnd) != nullptr) {
        return;
    }
    auto* state = new Win32ImeHookState();
    ::SetPropW(hwnd, kImeHookPropName, state);
    state->prev_wndproc = reinterpret_cast<WNDPROC>(::GetWindowLongPtrW(hwnd, GWLP_WNDPROC));
    ::SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(dynaxis_ime_wndproc));
    ImGui::GetPlatformIO().Platform_SetImeDataFn = dynaxis_set_ime_data;
}

void uninstall_win32_ime_hook(GLFWwindow* window) {
    if (!window) {
        return;
    }
    HWND hwnd = glfwGetWin32Window(window);
    if (!hwnd) {
        return;
    }
    Win32ImeHookState* state = get_ime_hook_state(hwnd);
    if (!state) {
        return;
    }
    if (state->prev_wndproc) {
        ::SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(state->prev_wndproc));
    }
    ::RemovePropW(hwnd, kImeHookPropName);
    delete state;
}
#endif

bool is_ime_composing(GLFWwindow* window) {
#if defined(_WIN32)
    Win32ImeHookState* state = get_ime_hook_state(window);
    return state != nullptr && (state->composing || state->candidate_open || state->preedit_len > 0);
#else
    (void)window;
    return false;
#endif
}

std::string format_timeline_line_for_copy(const ChatLogLine& item, const std::string& ts) {
    switch (item.kind) {
        case ChatLineKind::CHAT:
            return "[" + ts + "] " + (item.sender.empty() ? std::string("unknown") : item.sender) + ": " + item.text;
        case ChatLineKind::WHISPER_IN:
            return "[" + ts + "] [DM <- @" + item.sender + "] " + item.text;
        case ChatLineKind::WHISPER_OUT:
            return "[" + ts + "] [DM -> @" + item.recipient + "] " + item.text;
        case ChatLineKind::SYSTEM_NOTICE:
            return "[" + ts + "] [SYS] " + item.text;
        case ChatLineKind::ERROR_NOTICE:
            return "[" + ts + "] [ERR] " + item.text;
    }
    return "[" + ts + "] " + item.text;
}

void cancel_active_ime_composition(GLFWwindow* window) {
#if defined(_WIN32)
    Win32ImeHookState* state = get_ime_hook_state(window);
    if (!state) {
        return;
    }
    if (HWND hwnd = glfwGetWin32Window(window)) {
        if (HIMC himc = ::ImmGetContext(hwnd)) {
            if (::ImmGetCompositionStringW(himc, GCS_COMPSTR, nullptr, 0) > 0) {
                ::ImmNotifyIME(himc, NI_COMPOSITIONSTR, CPS_CANCEL, 0);
            }
            ::ImmReleaseContext(hwnd, himc);
        }
    }
    state->composing = false;
    state->comp_utf8.clear();
    state->comp_dirty = false;
    state->preedit_id = 0;
    state->preedit_pos = -1;
    state->preedit_len = 0;
    state->applied_preedit.clear();
#else
    (void)window;
#endif
}

void reconcile_ime_before_widgets(GLFWwindow* window) {
#if defined(_WIN32)
    Win32ImeHookState* state = get_ime_hook_state(window);
    ImGuiContext* ctx = GImGui;
    if (!state || !ctx || state->preedit_len <= 0) {
        return;
    }

    if (ctx->ActiveId == 0 || ctx->ActiveId != state->preedit_id) {
        cancel_active_ime_composition(window);
        return;
    }

    ImGuiInputTextState* ts = ImGui::GetInputTextState(state->preedit_id);
    if (!ts || !ts->TextA.Data ||
        state->preedit_pos < 0 ||
        state->preedit_pos + state->preedit_len > ts->TextLen ||
        static_cast<int>(state->applied_preedit.size()) != state->preedit_len ||
        std::memcmp(
            ts->TextA.Data + state->preedit_pos,
            state->applied_preedit.data(),
            static_cast<std::size_t>(state->preedit_len)) != 0) {
        state->preedit_id = 0;
        state->preedit_pos = -1;
        state->preedit_len = 0;
        state->applied_preedit.clear();
        return;
    }

    const ImGuiIO& io = ctx->IO;
    const bool mouse_interrupt = io.MouseClicked[0] || io.MouseClicked[1] || io.MouseClicked[2];
    const bool nav_interrupt = ImGui::IsKeyPressed(ImGuiKey_LeftArrow, false) ||
                               ImGui::IsKeyPressed(ImGuiKey_RightArrow, false) ||
                               ImGui::IsKeyPressed(ImGuiKey_UpArrow, false) ||
                               ImGui::IsKeyPressed(ImGuiKey_DownArrow, false) ||
                               ImGui::IsKeyPressed(ImGuiKey_Home, false) ||
                               ImGui::IsKeyPressed(ImGuiKey_End, false) ||
                               ImGui::IsKeyPressed(ImGuiKey_Delete, false) ||
                               ImGui::IsKeyPressed(ImGuiKey_Tab, false);
    const bool ctrl_interrupt = io.KeyCtrl && (ImGui::IsKeyPressed(ImGuiKey_A, false) ||
                                               ImGui::IsKeyPressed(ImGuiKey_C, false) ||
                                               ImGui::IsKeyPressed(ImGuiKey_X, false) ||
                                               ImGui::IsKeyPressed(ImGuiKey_V, false) ||
                                               ImGui::IsKeyPressed(ImGuiKey_Z, false) ||
                                               ImGui::IsKeyPressed(ImGuiKey_Y, false));
    if (mouse_interrupt || nav_interrupt || ctrl_interrupt) {
        // Leave the in-buffer syllable intact where it sits and cancel IMM so it is not re-emitted.
        cancel_active_ime_composition(window);
        return;
    }

    if (io.InputQueueCharacters.Size > 0 || (state->comp_dirty && !has_queued_text_events(*ctx))) {
        const int pos = state->preedit_pos;
        const int len = state->preedit_len;
        std::memmove(
            ts->TextA.Data + pos,
            ts->TextA.Data + pos + len,
            static_cast<std::size_t>(ts->TextLen - (pos + len) + 1));
        ts->TextLen -= len;
        ts->TextSrc = ts->TextA.Data;

        const int old_cursor = ts->GetCursorPos();
        int new_cursor = old_cursor;
        if (old_cursor >= pos + len) {
            new_cursor -= len;
        } else if (old_cursor > pos) {
            new_cursor = pos;
        }
        new_cursor = std::clamp(new_cursor, 0, ts->TextLen);
        ts->SetSelection(new_cursor, new_cursor);
        ts->CursorFollow = true;
        ts->CursorAnimReset();

        state->preedit_id = 0;
        state->preedit_pos = -1;
        state->preedit_len = 0;
        state->applied_preedit.clear();
    }
#else
    (void)window;
#endif
}

struct ImeInputCallbackContext {
    GLFWwindow* window{nullptr};
    GuiState* chat_state{nullptr};
    bool is_admin{false};
};

int unified_input_text_callback(ImGuiInputTextCallbackData* data) {
    auto* ctx = static_cast<ImeInputCallbackContext*>(data->UserData);
    if (!ctx) {
        return 0;
    }

    if (data->EventFlag == ImGuiInputTextFlags_CallbackCompletion) {
        if (ctx->chat_state != nullptr && data->Buf && data->Buf[0] == '/') {
            const std::string typed(data->Buf, static_cast<std::size_t>(data->BufTextLen));
            for (const auto& item : kSlashSuggestions) {
                if (item.admin_only && !ctx->is_admin) {
                    continue;
                }
                const std::string cmd_str(item.command);
                if (cmd_str.rfind(typed, 0) == 0 && typed.size() < cmd_str.size()) {
                    cancel_active_ime_composition(ctx->window);
                    data->DeleteChars(0, data->BufTextLen);
                    data->InsertChars(0, cmd_str.c_str());
                    data->CursorPos = data->BufTextLen;
                    data->SelectionStart = data->SelectionEnd = data->BufTextLen;
                    break;
                }
            }
        }
        return 0;
    }

    if (data->EventFlag == ImGuiInputTextFlags_CallbackHistory) {
        if (ctx->chat_state != nullptr &&
            (ctx->chat_state->input_history.size() > 0 || ctx->chat_state->input_history.navigating())) {
            cancel_active_ime_composition(ctx->window);
            const std::string current(data->Buf, static_cast<std::size_t>(data->BufTextLen));
            std::string recalled;
            if (data->EventKey == ImGuiKey_UpArrow) {
                recalled = ctx->chat_state->input_history.navigate_up(current);
            } else if (data->EventKey == ImGuiKey_DownArrow) {
                recalled = ctx->chat_state->input_history.navigate_down(current);
            } else {
                return 0;
            }
            data->DeleteChars(0, data->BufTextLen);
            if (!recalled.empty()) {
                data->InsertChars(0, recalled.c_str());
            }
            data->CursorPos = data->BufTextLen;
            data->SelectionStart = data->SelectionEnd = data->BufTextLen;
        }
        return 0;
    }

    if (data->EventFlag == ImGuiInputTextFlags_CallbackAlways) {
        const auto clamp_buffer_to_char_units = [](ImGuiInputTextCallbackData* d) {
            if (!d || !d->Buf || d->BufTextLen <= 0) {
                return;
            }
            const std::string_view buf_sv(d->Buf, static_cast<std::size_t>(d->BufTextLen));
            const std::size_t keep_bytes = utf8_prefix_bytes_for_char_units(buf_sv, kMaxChatCharUnits);
            if (keep_bytes < static_cast<std::size_t>(d->BufTextLen)) {
                const int keep_i = static_cast<int>(keep_bytes);
                d->DeleteChars(keep_i, d->BufTextLen - keep_i);
                d->CursorPos = std::clamp(d->CursorPos, 0, d->BufTextLen);
                d->SelectionStart = std::clamp(d->SelectionStart, 0, d->BufTextLen);
                d->SelectionEnd = std::clamp(d->SelectionEnd, 0, d->BufTextLen);
            }
        };

        if (ctx->chat_state != nullptr) {
            if (!ctx->chat_state->pending_chat_input.empty()) {
                cancel_active_ime_composition(ctx->window);
                data->DeleteChars(0, data->BufTextLen);
                data->InsertChars(0, ctx->chat_state->pending_chat_input.c_str());
                data->CursorPos = data->BufTextLen;
                data->SelectionStart = data->SelectionEnd = data->BufTextLen;
                ctx->chat_state->pending_chat_input.clear();
                ctx->chat_state->move_cursor_to_end = false;
            } else if (ctx->chat_state->move_cursor_to_end) {
                data->CursorPos = data->BufTextLen;
                data->SelectionStart = data->SelectionEnd = data->BufTextLen;
                ctx->chat_state->move_cursor_to_end = false;
            }
        }

        bool has_active_preedit = false;
#if defined(_WIN32)
        if (Win32ImeHookState* ime = get_ime_hook_state(ctx->window)) {
            if (GImGui != nullptr && !has_queued_text_events(*GImGui)) {
                if (ime->preedit_id == data->ID && ime->preedit_len > 0 && ime->comp_dirty &&
                    ime->preedit_pos >= 0 && ime->preedit_pos + ime->preedit_len <= data->BufTextLen) {
                    data->DeleteChars(ime->preedit_pos, ime->preedit_len);
                    ime->preedit_id = 0;
                    ime->preedit_pos = -1;
                    ime->preedit_len = 0;
                    ime->applied_preedit.clear();
                }

                if (ime->preedit_len == 0) {
                    if (ctx->chat_state != nullptr) {
                        clamp_buffer_to_char_units(data);
                    }
                    if (ime->composing && !ime->comp_utf8.empty()) {
                        if (data->HasSelection()) {
                            const int sel_min = std::min(data->SelectionStart, data->SelectionEnd);
                            const int sel_max = std::max(data->SelectionStart, data->SelectionEnd);
                            if (sel_max > sel_min) {
                                data->DeleteChars(sel_min, sel_max - sel_min);
                            }
                        }
                        std::string comp_to_insert = ime->comp_utf8;
                        if (ctx->chat_state != nullptr) {
                            const Utf8CharStats base_stats = measure_utf8_char_stats(
                                std::string_view(data->Buf, static_cast<std::size_t>(data->BufTextLen)));
                            const std::size_t rem_units = (base_stats.weighted_units < kMaxChatCharUnits)
                                                              ? (kMaxChatCharUnits - base_stats.weighted_units)
                                                              : 0;
                            const std::size_t allowed_comp_bytes =
                                utf8_prefix_bytes_for_char_units(ime->comp_utf8, rem_units);
                            if (allowed_comp_bytes == 0) {
                                comp_to_insert.clear();
                                cancel_active_ime_composition(ctx->window);
                            } else if (allowed_comp_bytes < comp_to_insert.size()) {
                                comp_to_insert.resize(allowed_comp_bytes);
                            }
                        }
                        if (!comp_to_insert.empty()) {
                            const int insert_pos = std::clamp(data->CursorPos, 0, data->BufTextLen);
                            const int before_len = data->BufTextLen;
                            data->InsertChars(insert_pos, comp_to_insert.c_str());
                            const int inserted_len = data->BufTextLen - before_len;
                            if (inserted_len > 0) {
                                ime->preedit_id = data->ID;
                                ime->preedit_pos = insert_pos;
                                ime->preedit_len = inserted_len;
                                ime->applied_preedit.assign(data->Buf + insert_pos, static_cast<std::size_t>(inserted_len));
                                data->CursorPos = insert_pos + inserted_len;
                                data->SelectionStart = data->SelectionEnd = data->CursorPos;
                            }
                        }
                        ime->comp_dirty = false;
                    } else if (ime->comp_dirty) {
                        ime->comp_dirty = false;
                    }
                }
            }
            has_active_preedit = (ime->preedit_len > 0 && ime->preedit_id == data->ID);
        }
#endif
        if (ctx->chat_state != nullptr && !has_active_preedit) {
            clamp_buffer_to_char_units(data);
        }
    }
    return 0;
}

bool input_text_ime(
    GLFWwindow* window,
    const char* label,
    char* buf,
    std::size_t buf_size,
    ImGuiInputTextFlags flags = 0) {
    ImeInputCallbackContext ctx{window, nullptr, false};
    return ImGui::InputText(
        label,
        buf,
        buf_size,
        flags | ImGuiInputTextFlags_CallbackAlways,
        unified_input_text_callback,
        &ctx);
}

bool input_text_with_hint_ime(
    GLFWwindow* window,
    const char* label,
    const char* hint,
    char* buf,
    std::size_t buf_size,
    ImGuiInputTextFlags flags = 0,
    GuiState* chat_state = nullptr,
    bool is_admin = false) {
    ImeInputCallbackContext ctx{window, chat_state, is_admin};
    return ImGui::InputTextWithHint(
        label,
        hint,
        buf,
        buf_size,
        flags | ImGuiInputTextFlags_CallbackAlways,
        unified_input_text_callback,
        &ctx);
}

} // namespace

GuiManager::GuiManager(ClientApp& app, UiMode initial_mode)
    : app_(app), mode_(initial_mode) {}

GuiManager::~GuiManager() {
    shutdown();
}

void GuiManager::set_mode(UiMode mode) {
    if (mode_ == mode) {
        return;
    }
    mode_ = mode;
    layout_initialized_ = false;
}

void GuiManager::toggle_mode() {
    set_mode(mode_ == UiMode::MESSENGER ? UiMode::WORKBENCH : UiMode::MESSENGER);
}

void GuiManager::set_text_zoom(float zoom) {
    text_zoom_ = std::clamp(zoom, kMinTextZoom, kMaxTextZoom);
    if (ImGui::GetCurrentContext() != nullptr) {
        ImGui::GetStyle().FontScaleMain = text_zoom_;
    }
}

void GuiManager::apply_preferences(const ClientPreferences& prefs, const std::filesystem::path& persist_path) {
    prefs_path_ = persist_path;

    ClientPreferences clean = prefs;
    sanitize_client_preferences(clean);

    std::snprintf(state_.host_buffer, sizeof(state_.host_buffer), "%s", clean.host.c_str());
    state_.port = clean.port;
    std::snprintf(state_.username_buffer, sizeof(state_.username_buffer), "%s", clean.username.c_str());
    state_.hide_blocked_messages = clean.hide_blocked_messages;
    set_text_zoom(clean.text_zoom);
    set_mode(clean.workbench_mode ? UiMode::WORKBENCH : UiMode::MESSENGER);
    app_.set_auto_resume_enabled(clean.auto_resume);
    app_.set_auto_ping_enabled(clean.auto_ping);
}

ClientPreferences GuiManager::current_preferences() const {
    ClientPreferences prefs;
    prefs.host = state_.host_buffer;
    prefs.port = state_.port;
    prefs.username = state_.username_buffer;
    prefs.workbench_mode = (mode_ == UiMode::WORKBENCH);
    prefs.text_zoom = text_zoom_;
    prefs.hide_blocked_messages = state_.hide_blocked_messages;
    prefs.auto_resume = app_.auto_resume_enabled();
    prefs.auto_ping = app_.auto_ping_enabled();
    sanitize_client_preferences(prefs);
    return prefs;
}

void GuiManager::save_preferences() const {
    if (prefs_path_.empty()) {
        return;
    }
    save_client_preferences_file(prefs_path_, current_preferences());
}

void GuiManager::apply_slate_dark_theme(float dpi_scale) {
    ImGuiStyle& style = ImGui::GetStyle();
    style = ImGuiStyle();
    ImGui::StyleColorsDark(&style);

    style.FontSizeBase = kBaseFontSizePx;
    style.WindowPadding = ImVec2(16.0f, 14.0f);
    style.FramePadding = ImVec2(11.0f, 7.0f);
    style.CellPadding = ImVec2(10.0f, 6.0f);
    style.ItemSpacing = ImVec2(10.0f, 8.0f);
    style.ItemInnerSpacing = ImVec2(8.0f, 6.0f);
    style.IndentSpacing = 18.0f;
    style.ScrollbarSize = 12.0f;
    style.GrabMinSize = 10.0f;
    style.DockingSeparatorSize = 2.0f;

    style.WindowRounding = 6.0f;
    style.ChildRounding = 7.0f;
    style.FrameRounding = 6.0f;
    style.PopupRounding = 6.0f;
    style.ScrollbarRounding = 6.0f;
    style.GrabRounding = 4.0f;
    style.TabRounding = 5.0f;
    style.WindowBorderSize = 1.0f;
    style.ChildBorderSize = 1.0f;
    style.FrameBorderSize = 0.0f;

    ImVec4* colors = style.Colors;
    colors[ImGuiCol_Text] = ImVec4(0.925f, 0.941f, 0.969f, 1.00f);
    colors[ImGuiCol_TextDisabled] = ImVec4(0.56f, 0.61f, 0.70f, 1.00f);
    colors[ImGuiCol_WindowBg] = ImVec4(0.086f, 0.094f, 0.114f, 1.00f);
    colors[ImGuiCol_ChildBg] = ImVec4(0.106f, 0.114f, 0.141f, 1.00f);
    colors[ImGuiCol_PopupBg] = ImVec4(0.118f, 0.125f, 0.157f, 0.98f);
    colors[ImGuiCol_Border] = ImVec4(0.165f, 0.180f, 0.224f, 1.00f);
    colors[ImGuiCol_BorderShadow] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    colors[ImGuiCol_FrameBg] = ImVec4(0.137f, 0.153f, 0.196f, 1.00f);
    colors[ImGuiCol_FrameBgHovered] = ImVec4(0.173f, 0.192f, 0.247f, 1.00f);
    colors[ImGuiCol_FrameBgActive] = ImVec4(0.204f, 0.231f, 0.302f, 1.00f);
    colors[ImGuiCol_TitleBg] = ImVec4(0.094f, 0.102f, 0.125f, 1.00f);
    colors[ImGuiCol_TitleBgActive] = ImVec4(0.118f, 0.125f, 0.157f, 1.00f);
    colors[ImGuiCol_TitleBgCollapsed] = ImVec4(0.086f, 0.094f, 0.114f, 1.00f);
    colors[ImGuiCol_MenuBarBg] = ImVec4(0.094f, 0.102f, 0.125f, 1.00f);
    colors[ImGuiCol_ScrollbarBg] = ImVec4(0.086f, 0.094f, 0.114f, 0.60f);
    colors[ImGuiCol_ScrollbarGrab] = ImVec4(0.220f, 0.243f, 0.310f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.280f, 0.314f, 0.400f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.340f, 0.380f, 0.486f, 1.00f);
    colors[ImGuiCol_CheckMark] = ImVec4(0.32f, 0.56f, 0.98f, 1.00f);
    colors[ImGuiCol_Button] = ImVec4(0.165f, 0.196f, 0.267f, 1.00f);
    colors[ImGuiCol_ButtonHovered] = ImVec4(0.216f, 0.267f, 0.369f, 1.00f);
    colors[ImGuiCol_ButtonActive] = ImVec4(0.259f, 0.329f, 0.471f, 1.00f);
    colors[ImGuiCol_Header] = ImVec4(0.153f, 0.192f, 0.271f, 1.00f);
    colors[ImGuiCol_HeaderHovered] = ImVec4(0.188f, 0.243f, 0.349f, 1.00f);
    colors[ImGuiCol_HeaderActive] = ImVec4(0.231f, 0.302f, 0.431f, 1.00f);
    colors[ImGuiCol_Separator] = ImVec4(0.165f, 0.180f, 0.224f, 1.00f);
    colors[ImGuiCol_Tab] = ImVec4(0.106f, 0.118f, 0.149f, 1.00f);
    colors[ImGuiCol_TabHovered] = ImVec4(0.188f, 0.243f, 0.349f, 1.00f);
    colors[ImGuiCol_TabSelected] = ImVec4(0.180f, 0.231f, 0.329f, 1.00f);
    colors[ImGuiCol_DockingPreview] = ImVec4(0.26f, 0.48f, 0.96f, 0.45f);
    colors[ImGuiCol_DockingEmptyBg] = ImVec4(0.086f, 0.094f, 0.114f, 1.00f);

    style.ScaleAllSizes(dpi_scale);
    style.FontScaleDpi = dpi_scale;
    style.FontScaleMain = text_zoom_;
}

void GuiManager::load_fonts() {
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->TexMinWidth = 1024;
    io.Fonts->TexMinHeight = 1024;

    bool font_loaded = false;
    const auto try_load = [&io](const std::filesystem::path& path, bool merge_mode) -> bool {
        std::error_code ec;
        if (path.empty() || !std::filesystem::is_regular_file(path, ec)) {
            return false;
        }
        ImFontConfig cfg = make_crisp_font_config(merge_mode);
        return io.Fonts->AddFontFromFileTTF(path.string().c_str(), kBaseFontSizePx, &cfg) != nullptr;
    };

    if (const char* custom_font = std::getenv("DYNAXIS_FONT_PATH")) {
        font_loaded = try_load(std::filesystem::path(custom_font), false);
    }

    if (!font_loaded) {
#if defined(_WIN32)
        char font_dir[MAX_PATH]{};
        if (SUCCEEDED(SHGetFolderPathA(nullptr, CSIDL_FONTS, nullptr, 0, font_dir))) {
            const std::filesystem::path dir(font_dir);
            const std::filesystem::path segoe = dir / "segoeui.ttf";
            const std::filesystem::path malgun = dir / "malgun.ttf";
            if (try_load(segoe, false)) {
                font_loaded = true;
                (void)try_load(malgun, true);
            } else if (try_load(malgun, false)) {
                font_loaded = true;
            }
        }
#elif defined(__APPLE__)
        font_loaded = try_load("/System/Library/Fonts/AppleSDGothicNeo.ttc", false);
#else
        const std::array<const char*, 3> linux_candidates{{
            "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
            "/usr/share/fonts/truetype/noto/NotoSansCJK-Regular.ttc",
            "/usr/share/fonts/google-noto-cjk/NotoSansCJK-Regular.ttc",
        }};
        for (const char* candidate : linux_candidates) {
            if (try_load(candidate, false)) {
                font_loaded = true;
                break;
            }
        }
#endif
    }

    if (!font_loaded) {
        ImFontConfig fallback_cfg = make_crisp_font_config(false);
        io.Fonts->AddFontDefaultVector(&fallback_cfg);
    }
}

bool GuiManager::init(int width, int height, const std::string& title) {
    glfwSetErrorCallback(glfw_error_callback);
    if (!glfwInit()) {
        return false;
    }

    dpi_scale_ = 1.0f;
    if (GLFWmonitor* primary = glfwGetPrimaryMonitor()) {
        const float detected_scale = ImGui_ImplGlfw_GetContentScaleForMonitor(primary);
        if (detected_scale >= 0.75f && detected_scale <= 4.0f) {
            dpi_scale_ = detected_scale;
        }
    }

#if defined(__APPLE__)
    const char* glsl_version = "#version 150";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
#else
    const char* glsl_version = "#version 130";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
#endif

    const int scaled_w = std::max(640, static_cast<int>(std::round(static_cast<float>(width) * dpi_scale_)));
    const int scaled_h = std::max(480, static_cast<int>(std::round(static_cast<float>(height) * dpi_scale_)));
    window_ = glfwCreateWindow(scaled_w, scaled_h, title.c_str(), nullptr, nullptr);
    if (window_ == nullptr) {
        glfwTerminate();
        return false;
    }

    glfwMakeContextCurrent(window_);
    glfwSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigInputTextEnterKeepActive = true;
    io.ConfigDpiScaleFonts = true;
    io.ConfigDpiScaleViewports = true;

    apply_slate_dark_theme(dpi_scale_);

    ImGui_ImplGlfw_InitForOpenGL(window_, true);
    ImGui_ImplOpenGL3_Init(glsl_version);

#if defined(_WIN32)
    install_win32_ime_hook(window_);
#endif

    load_fonts();
    return true;
}

void GuiManager::shutdown() {
    if (window_) {
        save_preferences();
#if defined(_WIN32)
        uninstall_win32_ime_hook(window_);
#endif
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
        glfwDestroyWindow(window_);
        glfwTerminate();
        window_ = nullptr;
    }
}

void GuiManager::run(unsigned int frame_limit) {
    if (!window_) {
        return;
    }

    const ImVec4 clear_color(0.086f, 0.094f, 0.114f, 1.00f);
    unsigned int rendered_frames = 0;

    while (!glfwWindowShouldClose(window_)) {
        glfwPollEvents();
        app_.update();

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        reconcile_ime_before_widgets(window_);

        if (ImGui::IsKeyPressed(ImGuiKey_F12, false)) {
            toggle_mode();
        }
        const ImGuiIO& io = ImGui::GetIO();
        if (io.KeyCtrl) {
            if (ImGui::IsKeyPressed(ImGuiKey_F, false)) {
                state_.show_timeline_search = true;
                state_.focus_timeline_search = true;
            } else if (ImGui::IsKeyPressed(ImGuiKey_Equal, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadAdd, false)) {
                set_text_zoom(text_zoom_ + 0.10f);
            } else if (ImGui::IsKeyPressed(ImGuiKey_Minus, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadSubtract, false)) {
                set_text_zoom(text_zoom_ - 0.10f);
            } else if (ImGui::IsKeyPressed(ImGuiKey_0, false) || ImGui::IsKeyPressed(ImGuiKey_Keypad0, false)) {
                set_text_zoom(1.0f);
            }
        }

        render_root_layout();
        render_connection_popup();
        render_navigation_panel();
        render_chat_panel();
        render_participants_panel();
        if (mode_ == UiMode::WORKBENCH) {
            render_diagnostics_panel();
        }

        ImGui::Render();
        int display_w = 0;
        int display_h = 0;
        glfwGetFramebufferSize(window_, &display_w, &display_h);
        glViewport(0, 0, display_w, display_h);
        glClearColor(clear_color.x, clear_color.y, clear_color.z, clear_color.w);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        glfwSwapBuffers(window_);
        if (frame_limit && ++rendered_frames >= frame_limit) {
            break;
        }
    }
}

void GuiManager::render_root_layout() {
    ImGuiWindowFlags host_flags = ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoDocking |
                                  ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
                                  ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                  ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::SetNextWindowViewport(viewport->ID);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
    ImGui::Begin("DynaxisMainHost", nullptr, host_flags);
    ImGui::PopStyleVar(4);

    render_menubar();
    render_top_bar();

    const ImGuiID dockspace_id = ImGui::GetID("DynaxisDockSpace");
    const ImVec2 dock_avail = ImGui::GetContentRegionAvail();

    if (!layout_initialized_ || ImGui::DockBuilderGetNode(dockspace_id) == nullptr) {
        if (dock_avail.x > 0.0f && dock_avail.y > 0.0f) {
            ImGui::DockBuilderRemoveNode(dockspace_id);
            ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);
            ImGui::DockBuilderSetNodeSize(dockspace_id, dock_avail);

            ImGuiID dock_main_id = dockspace_id;
            ImGuiID dock_left = ImGui::DockBuilderSplitNode(dock_main_id, ImGuiDir_Left, 0.22f, nullptr, &dock_main_id);
            ImGuiID dock_right = ImGui::DockBuilderSplitNode(dock_main_id, ImGuiDir_Right, 0.26f, nullptr, &dock_main_id);
            ImGuiID dock_right_bottom = 0;
            if (mode_ == UiMode::WORKBENCH) {
                dock_right_bottom = ImGui::DockBuilderSplitNode(dock_right, ImGuiDir_Down, 0.45f, nullptr, &dock_right);
            }

            if (mode_ == UiMode::MESSENGER) {
                const int locked_flags = ImGuiDockNodeFlags_NoTabBar |
                                         ImGuiDockNodeFlags_NoWindowMenuButton |
                                         ImGuiDockNodeFlags_NoUndocking |
                                         ImGuiDockNodeFlags_NoDockingSplit;
                auto apply_locked = [locked_flags](ImGuiID node_id) {
                    if (ImGuiDockNode* node = ImGui::DockBuilderGetNode(node_id)) {
                        node->LocalFlags |= locked_flags;
                    }
                };
                apply_locked(dock_left);
                apply_locked(dock_main_id);
                apply_locked(dock_right);
            }

            ImGui::DockBuilderDockWindow("Navigation", dock_left);
            ImGui::DockBuilderDockWindow("Chat", dock_main_id);
            ImGui::DockBuilderDockWindow("Participants", dock_right);
            if (mode_ == UiMode::WORKBENCH && dock_right_bottom != 0) {
                ImGui::DockBuilderDockWindow("Diagnostics", dock_right_bottom);
            }

            ImGui::DockBuilderFinish(dockspace_id);
            layout_initialized_ = true;
        }
    }

    ImGuiDockNodeFlags dockspace_flags = ImGuiDockNodeFlags_None;
    if (mode_ == UiMode::MESSENGER) {
        dockspace_flags |= ImGuiDockNodeFlags_NoUndocking | ImGuiDockNodeFlags_NoDockingSplit;
    }
    ImGui::DockSpace(dockspace_id, ImVec2(0.0f, 0.0f), dockspace_flags);
    ImGui::End();
}

void GuiManager::render_menubar() {
    if (!ImGui::BeginMenuBar()) {
        return;
    }

    if (ImGui::BeginMenu("Connection")) {
        if (ImGui::MenuItem("Connection Settings...")) {
            state_.show_connection_popup = true;
        }
        if (ImGui::MenuItem("Resume Last Session", nullptr, false, app_.can_resume())) {
            app_.reconnect_and_login(
                state_.host_buffer,
                state_.port,
                state_.username_buffer,
                state_.access_token_buffer,
                true);
        }
        if (ImGui::MenuItem("Disconnect", nullptr, false, app_.get_data().is_connected)) {
            app_.disconnect();
        }
        ImGui::Separator();
        bool auto_resume = app_.auto_resume_enabled();
        if (ImGui::MenuItem("Auto-Resume on Disconnect", nullptr, &auto_resume)) {
            app_.set_auto_resume_enabled(auto_resume);
            save_preferences();
        }
        if (ImGui::MenuItem("Save Preferences", nullptr, false, !prefs_path_.empty())) {
            save_preferences();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Exit")) {
            glfwSetWindowShouldClose(window_, true);
        }
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("View")) {
        const bool is_workbench = (mode_ == UiMode::WORKBENCH);
        if (ImGui::MenuItem("Developer Workbench", "F12", is_workbench)) {
            toggle_mode();
        }
        if (ImGui::MenuItem("Find in Timeline", "Ctrl+F", state_.show_timeline_search)) {
            state_.show_timeline_search = !state_.show_timeline_search;
            if (state_.show_timeline_search) {
                state_.focus_timeline_search = true;
            }
        }
        const bool whispers_only = (state_.timeline_filter == TimelineFilter::WHISPERS);
        if (ImGui::MenuItem("Whispers Only", nullptr, whispers_only)) {
            if (whispers_only) {
                state_.timeline_filter = TimelineFilter::ALL;
            } else {
                state_.timeline_filter = TimelineFilter::WHISPERS;
                state_.selected_whisper_peer.clear();
                app_.mark_all_whispers_read();
            }
            state_.auto_scroll = true;
            state_.has_unread_below = false;
        }
        ImGui::MenuItem("Hide Blocked Messages", nullptr, &state_.hide_blocked_messages);
        if (ImGui::MenuItem("Reset Layout")) {
            layout_initialized_ = false;
            ImGui::DockBuilderRemoveNode(ImGui::GetID("DynaxisDockSpace"));
        }
        if (ImGui::BeginMenu("Text Size")) {
            const auto zoom_item = [this](const char* label, float target) {
                const bool active = std::fabs(text_zoom_ - target) < 0.03f;
                if (ImGui::MenuItem(label, nullptr, active)) {
                    set_text_zoom(target);
                }
            };
            zoom_item("90%", 0.90f);
            zoom_item("100% (Default)", 1.00f);
            zoom_item("110%", 1.10f);
            zoom_item("125%", 1.25f);
            ImGui::Separator();
            if (ImGui::MenuItem("Increase", "Ctrl+=")) {
                set_text_zoom(text_zoom_ + 0.10f);
            }
            if (ImGui::MenuItem("Decrease", "Ctrl+-")) {
                set_text_zoom(text_zoom_ - 0.10f);
            }
            if (ImGui::MenuItem("Reset", "Ctrl+0")) {
                set_text_zoom(1.0f);
            }
            ImGui::EndMenu();
        }
        ImGui::EndMenu();
    }

    ImGui::EndMenuBar();
}

void GuiManager::render_top_bar() {
    const auto& data = app_.get_data();
    const ImGuiStyle& style = ImGui::GetStyle();
    const float pad_x = std::round(16.0f * dpi_scale_);
    const float pad_y = std::round(10.0f * dpi_scale_);
    const float bar_height = ImGui::GetFrameHeight() + pad_y * 2.0f;

    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.106f, 0.118f, 0.149f, 1.00f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(pad_x, pad_y));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 0.0f);
    ImGui::BeginChild(
        "TopStatusBar",
        ImVec2(0.0f, bar_height),
        ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    ImGui::AlignTextToFramePadding();
    const float frame_h = ImGui::GetFrameHeight();
    const float dot_radius = std::max(4.0f, 4.2f * dpi_scale_ * text_zoom_);
    const ImVec2 dot_origin = ImGui::GetCursorScreenPos();
    const ImVec2 dot_center(dot_origin.x + dot_radius, dot_origin.y + frame_h * 0.5f);
    ImGui::GetWindowDrawList()->AddCircleFilled(
        dot_center,
        dot_radius,
        ImGui::GetColorU32(transport_state_color(data.connection_state)));
    ImGui::Dummy(ImVec2(dot_radius * 2.0f + 2.0f, frame_h));
    ImGui::SameLine(0.0f, style.ItemInnerSpacing.x);
    ImGui::TextColored(transport_state_color(data.connection_state), "%s", transport_state_label(data.connection_state));
    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();

    if (mode_ == UiMode::WORKBENCH || data.is_connected) {
        const std::string& active_host = data.last_host.empty() ? std::string(state_.host_buffer) : data.last_host;
        const int active_port = data.last_port > 0 ? data.last_port : state_.port;
        ImGui::Text("%s:%d", active_host.c_str(), active_port);
        ImGui::SameLine();
    }

    if (data.is_logged_in) {
        const std::string& shown_name = data.session.display_name.empty()
                                            ? data.session.effective_user
                                            : data.session.display_name;
        ImGui::TextDisabled("|");
        ImGui::SameLine();
        ImGui::TextUnformatted(shown_name.c_str());
        if (!data.session.effective_user.empty() && data.session.effective_user != shown_name) {
            ImGui::SameLine();
            ImGui::TextDisabled("(@%s)", data.session.effective_user.c_str());
        }
        if (data.is_admin) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.96f, 0.72f, 0.30f, 1.0f), "[ADMIN]");
        }
    }

    if (mode_ == UiMode::WORKBENCH && data.is_connected) {
        ImGui::SameLine();
        ImGui::TextDisabled("|");
        ImGui::SameLine();
        if (data.diagnostics.has_rtt) {
            ImGui::TextDisabled(
                "RTT %u ms (avg %u)",
                data.diagnostics.last_rtt_ms,
                data.diagnostics.avg_rtt_ms);
        } else {
            ImGui::TextDisabled("RTT --");
        }
    }

    // Dynamically size right-aligned controls from label metrics so text never clips at any zoom/DPI.
    const bool show_resume = !data.is_connected && app_.can_resume();
    const bool workbench_active = (mode_ == UiMode::WORKBENCH);
    const char* workbench_label = workbench_active ? "Workbench: ON" : "Workbench";

    const float conn_w = calc_button_width("Connection...", 8.0f);
    const float resume_w = show_resume ? calc_button_width("Resume Session", 8.0f) : 0.0f;
    const float disc_w = data.is_connected ? calc_button_width("Disconnect", 8.0f) : 0.0f;
    const float wb_w = calc_button_width(workbench_label, 10.0f);

    float right_width = conn_w + style.ItemSpacing.x + wb_w;
    if (show_resume) {
        right_width += resume_w + style.ItemSpacing.x;
    }
    if (data.is_connected) {
        right_width += disc_w + style.ItemSpacing.x;
    }

    const float avail = ImGui::GetContentRegionAvail().x;
    if (avail > right_width) {
        ImGui::SameLine(ImGui::GetCursorPosX() + (avail - right_width));
    } else {
        ImGui::SameLine();
    }

    if (ImGui::Button("Connection...", ImVec2(conn_w, frame_h))) {
        state_.show_connection_popup = true;
    }
    if (show_resume) {
        ImGui::SameLine();
        push_accent_button_style();
        if (ImGui::Button("Resume Session", ImVec2(resume_w, frame_h))) {
            app_.reconnect_and_login(
                state_.host_buffer,
                state_.port,
                state_.username_buffer,
                state_.access_token_buffer,
                true);
        }
        pop_accent_button_style();
    }
    if (data.is_connected) {
        ImGui::SameLine();
        if (ImGui::Button("Disconnect", ImVec2(disc_w, frame_h))) {
            app_.disconnect();
        }
    }
    ImGui::SameLine();
    if (workbench_active) {
        push_accent_button_style();
    }
    if (ImGui::Button(workbench_label, ImVec2(wb_w, frame_h))) {
        toggle_mode();
    }
    if (workbench_active) {
        pop_accent_button_style();
    }

    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
}

void GuiManager::render_connection_popup() {
    if (state_.show_connection_popup) {
        ImGui::OpenPopup("Connection Settings");
        state_.show_connection_popup = false;
    }

    const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

    bool open = true;
    if (ImGui::BeginPopupModal("Connection Settings", &open, ImGuiWindowFlags_AlwaysAutoResize)) {
        const float field_w = std::round(280.0f * dpi_scale_);

        ImGui::TextUnformatted("Connect to Dynaxis Gateway / HAProxy");
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        ImGui::SetNextItemWidth(field_w);
        input_text_ime(window_, "Host", state_.host_buffer, sizeof(state_.host_buffer));

        ImGui::SetNextItemWidth(field_w);
        ImGui::InputInt("Port", &state_.port);

        ImGui::SetNextItemWidth(field_w);
        const bool user_enter = input_text_ime(
            window_,
            "Username",
            state_.username_buffer,
            sizeof(state_.username_buffer),
            ImGuiInputTextFlags_EnterReturnsTrue);

        ImGui::SetNextItemWidth(field_w);
        const bool token_enter = ImGui::InputText(
            "Access token",
            state_.access_token_buffer,
            sizeof(state_.access_token_buffer),
            ImGuiInputTextFlags_Password | ImGuiInputTextFlags_EnterReturnsTrue);

        ImGui::Spacing();
        ImGui::Checkbox("Resume previous session", &state_.resume_session);
        if (state_.resume_session && !app_.can_resume()) {
            ImGui::SameLine();
            ImGui::TextDisabled("(no saved token yet)");
        }
        bool auto_resume = app_.auto_resume_enabled();
        if (ImGui::Checkbox("Auto-resume on unexpected disconnect", &auto_resume)) {
            app_.set_auto_resume_enabled(auto_resume);
        }
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        push_accent_button_style();
        const float connect_w = calc_button_width("Connect & Login", 14.0f);
        const bool connect_clicked = ImGui::Button("Connect & Login", ImVec2(connect_w, 0.0f));
        pop_accent_button_style();
        if (connect_clicked || user_enter || token_enter) {
            cancel_active_ime_composition(window_);
            save_preferences();
            if (app_.reconnect_and_login(
                    state_.host_buffer,
                    state_.port,
                    state_.username_buffer,
                    state_.access_token_buffer,
                    state_.resume_session)) {
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::SameLine();
        const float cancel_w = calc_button_width("Cancel", 14.0f);
        if (ImGui::Button("Cancel", ImVec2(cancel_w, 0.0f))) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void GuiManager::render_navigation_panel() {
    ImGui::Begin(
        "Navigation",
        nullptr,
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    const auto& data = app_.get_data();
    const ImGuiStyle& style = ImGui::GetStyle();
    const float row_h = selectable_row_height();
    const ImVec2 list_pad(std::round(10.0f * dpi_scale_), std::round(8.0f * dpi_scale_));

    ImGui::TextDisabled("ROOMS");

    const float new_btn_width = calc_button_width("+ New", 8.0f);
    ImGui::SetNextItemWidth(std::max(80.0f, ImGui::GetContentRegionAvail().x - new_btn_width - style.ItemSpacing.x));
    input_text_with_hint_ime(
        window_,
        "##RoomSearch",
        "Filter rooms...",
        state_.room_search_buffer,
        sizeof(state_.room_search_buffer));
    ImGui::SameLine();
    if (ImGui::Button("+ New", ImVec2(new_btn_width, 0.0f))) {
        state_.show_create_room_modal = true;
        state_.new_room_buffer[0] = '\0';
        state_.password_buffer[0] = '\0';
    }

    if (state_.show_create_room_modal) {
        ImGui::OpenPopup("Create Room");
        state_.show_create_room_modal = false;
    }
    if (ImGui::BeginPopupModal("Create Room", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        const float field_w = std::round(240.0f * dpi_scale_);
        ImGui::SetNextItemWidth(field_w);
        const bool room_enter = input_text_ime(
            window_,
            "Room Name",
            state_.new_room_buffer,
            sizeof(state_.new_room_buffer),
            ImGuiInputTextFlags_EnterReturnsTrue);

        ImGui::SetNextItemWidth(field_w);
        const bool pass_enter = ImGui::InputText(
            "Password (Optional)",
            state_.password_buffer,
            sizeof(state_.password_buffer),
            ImGuiInputTextFlags_Password | ImGuiInputTextFlags_EnterReturnsTrue);

        ImGui::Separator();

        push_accent_button_style();
        const float create_w = calc_button_width("Create & Join", 12.0f);
        const bool create_clicked = ImGui::Button("Create & Join", ImVec2(create_w, 0.0f));
        pop_accent_button_style();
        if ((create_clicked || room_enter || pass_enter) && state_.new_room_buffer[0] != '\0') {
            cancel_active_ime_composition(window_);
            state_.timeline_filter = TimelineFilter::ALL;
            state_.selected_whisper_peer.clear();
            state_.auto_scroll = true;
            state_.has_unread_below = false;
            app_.join_room(state_.new_room_buffer, state_.password_buffer);
            state_.new_room_buffer[0] = '\0';
            state_.password_buffer[0] = '\0';
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        const float cancel_w = calc_button_width("Cancel", 12.0f);
        if (ImGui::Button("Cancel", ImVec2(cancel_w, 0.0f))) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    float peer_list_h = 0.0f;
    float peer_section_h = 0.0f;
    if (!data.whisper_peers.empty()) {
        const int visible_peers = std::min(4, static_cast<int>(data.whisper_peers.size()));
        peer_list_h = list_pad.y * 2.0f +
                      static_cast<float>(visible_peers) * row_h +
                      static_cast<float>(std::max(0, visible_peers - 1)) * style.ItemSpacing.y;
        peer_section_h = (style.SeparatorSize + style.ItemSpacing.y) +
                         ImGui::GetTextLineHeightWithSpacing() +
                         (peer_list_h + style.ItemSpacing.y);
    }
    const float quick_join_section_h = (style.SeparatorSize + style.ItemSpacing.y) +
                                       ImGui::GetTextLineHeightWithSpacing() +
                                       ImGui::GetFrameHeight();
    const float footer_height = style.ItemSpacing.y + peer_section_h + quick_join_section_h;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, list_pad);
    ImGui::BeginChild("RoomListRegion", ImVec2(0.0f, -footer_height), ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::PushStyleVar(ImGuiStyleVar_SelectableTextAlign, ImVec2(0.0f, 0.5f));

    if (data.is_connected) {
        for (const auto& r : data.room_list) {
            if (!contains_case_insensitive(r.name, state_.room_search_buffer)) {
                continue;
            }
            std::string label = "# " + r.name;
            if (r.locked) {
                label += "  [locked]";
            }
            label += "###room_" + r.name;
            const bool is_current = (r.name == data.current_room);
            if (ImGui::Selectable(label.c_str(), is_current, ImGuiSelectableFlags_None, ImVec2(0.0f, row_h))) {
                state_.timeline_filter = TimelineFilter::ALL;
                state_.selected_whisper_peer.clear();
                state_.auto_scroll = true;
                state_.has_unread_below = false;
                if (r.locked) {
                    state_.target_room_to_join = r.name;
                    state_.show_password_modal = true;
                } else {
                    app_.join_room(r.name, "");
                }
            }
            if (ImGui::BeginPopupContextItem()) {
                if (ImGui::MenuItem("Join Room")) {
                    state_.timeline_filter = TimelineFilter::ALL;
                    state_.selected_whisper_peer.clear();
                    state_.auto_scroll = true;
                    state_.has_unread_below = false;
                    if (r.locked) {
                        state_.target_room_to_join = r.name;
                        state_.show_password_modal = true;
                    } else {
                        app_.join_room(r.name, "");
                    }
                }
                if (r.name != "lobby" && ImGui::MenuItem("Remove Room", nullptr, false, data.is_logged_in)) {
                    app_.process_command("/room remove " + r.name);
                }
                ImGui::EndPopup();
            }
        }
        if (data.room_list.empty()) {
            ImGui::TextDisabled("No active rooms");
        }
    } else {
        ImGui::TextDisabled("Connect to load rooms");
    }

    ImGui::PopStyleVar();
    ImGui::EndChild();
    ImGui::PopStyleVar();

    if (!data.whisper_peers.empty()) {
        ImGui::Separator();
        ImGui::TextDisabled("RECENT WHISPERS");
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, list_pad);
        ImGui::BeginChild("WhisperPeersRegion", ImVec2(0.0f, peer_list_h), ImGuiChildFlags_AlwaysUseWindowPadding);
        ImGui::PushStyleVar(ImGuiStyleVar_SelectableTextAlign, ImVec2(0.0f, 0.5f));
        for (const auto& peer : data.whisper_peers) {
            const bool selected =
                (state_.timeline_filter == TimelineFilter::WHISPER_PEER && state_.selected_whisper_peer == peer) ||
                (state_.timeline_filter != TimelineFilter::WHISPER_PEER && state_.active_whisper_target == peer);
            const std::string label = "@ " + peer + "###whisper_peer_" + peer;
            if (ImGui::Selectable(label.c_str(), selected, ImGuiSelectableFlags_None, ImVec2(0.0f, row_h))) {
                state_.selected_whisper_peer = peer;
                state_.timeline_filter = TimelineFilter::WHISPER_PEER;
                state_.active_whisper_target = peer;
                state_.auto_scroll = true;
                state_.has_unread_below = false;
                state_.focus_chat_input = true;
                state_.move_cursor_to_end = true;
                app_.mark_whisper_peer_read(peer);
            }
            if (ImGui::BeginPopupContextItem()) {
                if (ImGui::MenuItem("Open 1:1 Thread")) {
                    state_.selected_whisper_peer = peer;
                    state_.timeline_filter = TimelineFilter::WHISPER_PEER;
                    state_.active_whisper_target = peer;
                    state_.auto_scroll = true;
                    state_.has_unread_below = false;
                    state_.focus_chat_input = true;
                    state_.move_cursor_to_end = true;
                    app_.mark_whisper_peer_read(peer);
                }
                if (ImGui::MenuItem("Whisper in Current Room View")) {
                    state_.timeline_filter = TimelineFilter::ALL;
                    state_.selected_whisper_peer.clear();
                    state_.active_whisper_target = peer;
                    state_.focus_chat_input = true;
                    state_.move_cursor_to_end = true;
                    app_.mark_whisper_peer_read(peer);
                }
                const std::uint32_t unread_ctx = app_.unread_whisper_count(peer);
                if (ImGui::MenuItem("Mark Read", nullptr, false, unread_ctx > 0)) {
                    app_.mark_whisper_peer_read(peer);
                }
                ImGui::Separator();
                if (app_.is_user_blocked(peer)) {
                    if (ImGui::MenuItem("Unblock", nullptr, false, data.is_logged_in)) {
                        app_.unblock_user(peer);
                    }
                } else {
                    if (ImGui::MenuItem("Block", nullptr, false, data.is_logged_in)) {
                        app_.block_user(peer);
                    }
                }
                ImGui::EndPopup();
            }

            const std::uint32_t unread = app_.unread_whisper_count(peer);
            if (unread > 0) {
                const ImVec2 item_min = ImGui::GetItemRectMin();
                const ImVec2 item_max = ImGui::GetItemRectMax();
                const std::string badge_text = (unread > 99) ? "99+" : std::to_string(unread);
                const ImVec2 text_sz = ImGui::CalcTextSize(badge_text.c_str());
                const float pad_x = std::round(6.0f * dpi_scale_);
                const float pill_w = std::max(text_sz.x + pad_x * 2.0f, ImGui::GetTextLineHeight() + 2.0f);
                const float pill_h = std::min(item_max.y - item_min.y - 2.0f, ImGui::GetTextLineHeight() + std::round(3.0f * dpi_scale_));
                const float pill_x2 = item_max.x - std::round(6.0f * dpi_scale_);
                const float pill_x1 = pill_x2 - pill_w;
                const float pill_y1 = item_min.y + ((item_max.y - item_min.y) - pill_h) * 0.5f;
                const float pill_y2 = pill_y1 + pill_h;
                ImDrawList* dl = ImGui::GetWindowDrawList();
                dl->AddRectFilled(
                    ImVec2(pill_x1, pill_y1),
                    ImVec2(pill_x2, pill_y2),
                    IM_COL32(59, 130, 246, 235),
                    pill_h * 0.5f);
                const ImVec2 txt_pos(
                    pill_x1 + (pill_w - text_sz.x) * 0.5f,
                    pill_y1 + (pill_h - text_sz.y) * 0.5f);
                dl->AddText(txt_pos, IM_COL32(255, 255, 255, 255), badge_text.c_str());
            }
        }
        ImGui::PopStyleVar();
        ImGui::EndChild();
        ImGui::PopStyleVar();
    }

    ImGui::Separator();
    ImGui::TextDisabled("QUICK JOIN");
    const float go_width = calc_button_width("Go", 8.0f);
    ImGui::SetNextItemWidth(std::max(80.0f, ImGui::GetContentRegionAvail().x - go_width - style.ItemSpacing.x));
    const bool quick_submit = input_text_with_hint_ime(
        window_,
        "##QuickJoin",
        "Enter room name...",
        state_.quick_join_buffer,
        sizeof(state_.quick_join_buffer),
        ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    const bool go_clicked = ImGui::Button("Go", ImVec2(go_width, 0.0f));
    if ((go_clicked || quick_submit) && state_.quick_join_buffer[0] != '\0') {
        cancel_active_ime_composition(window_);
        state_.timeline_filter = TimelineFilter::ALL;
        state_.selected_whisper_peer.clear();
        state_.auto_scroll = true;
        state_.has_unread_below = false;
        app_.join_room(state_.quick_join_buffer, "");
        state_.quick_join_buffer[0] = '\0';
    }

    if (state_.show_password_modal) {
        ImGui::OpenPopup("Join Locked Room");
        state_.show_password_modal = false;
        state_.password_buffer[0] = '\0';
    }
    if (ImGui::BeginPopupModal("Join Locked Room", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Enter password for #%s", state_.target_room_to_join.c_str());
        ImGui::Spacing();
        if (ImGui::IsWindowAppearing()) {
            ImGui::SetKeyboardFocusHere(0);
        }
        ImGui::SetNextItemWidth(std::round(240.0f * dpi_scale_));
        const bool enter_pressed = ImGui::InputText(
            "Password",
            state_.password_buffer,
            sizeof(state_.password_buffer),
            ImGuiInputTextFlags_Password | ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        push_accent_button_style();
        const float join_w = calc_button_width("Join", 14.0f);
        if (ImGui::Button("Join", ImVec2(join_w, 0.0f)) || enter_pressed) {
            app_.join_room(state_.target_room_to_join, state_.password_buffer);
            state_.password_buffer[0] = '\0';
            ImGui::CloseCurrentPopup();
        }
        pop_accent_button_style();
        ImGui::SameLine();
        const float cancel_w = calc_button_width("Cancel", 14.0f);
        if (ImGui::Button("Cancel", ImVec2(cancel_w, 0.0f))) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    ImGui::End();
}

void GuiManager::render_empty_timeline_card(bool has_search) {
    const auto& data = app_.get_data();
    const float pad_x = std::round(16.0f * dpi_scale_);
    const float pad_y = std::round(14.0f * dpi_scale_);

    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.118f, 0.129f, 0.165f, 1.00f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(pad_x, pad_y));
    ImGui::BeginChild(
        "EmptyTimelineCard",
        ImVec2(0.0f, 0.0f),
        ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding,
        ImGuiWindowFlags_NoScrollbar);

    if (has_search) {
        ImGui::TextUnformatted("No messages match the current search.");
        ImGui::Spacing();
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("Query: \"%s\"", state_.timeline_search_buffer);
        ImGui::PopTextWrapPos();
        ImGui::Spacing();
        const float clear_w = calc_button_width("Clear Search", 12.0f);
        if (ImGui::Button("Clear Search", ImVec2(clear_w, 0.0f))) {
            state_.timeline_search_buffer[0] = '\0';
        }
    } else if (!data.is_connected && data.timeline.lines.empty()) {
        ImGui::TextUnformatted("Welcome to Dynaxis Messenger");
        ImGui::Spacing();
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("1. Connect to your gateway/HAProxy endpoint via Quick Connect above.");
        ImGui::TextDisabled("2. Join rooms with /join <room>, or send private messages with /w <user> <text>.");
        ImGui::TextDisabled("3. Shortcuts: Ctrl+F search timeline  |  Up/Down input history  |  F12 Developer Workbench");
        ImGui::PopTextWrapPos();
    } else if (state_.timeline_filter == TimelineFilter::WHISPER_PEER && !state_.selected_whisper_peer.empty()) {
        ImGui::Text("No whispers with @%s yet.", state_.selected_whisper_peer.c_str());
        ImGui::Spacing();
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("Send a message below to start a 1:1 whisper conversation.");
        ImGui::PopTextWrapPos();
    } else if (state_.timeline_filter == TimelineFilter::WHISPERS) {
        ImGui::TextUnformatted("No whisper messages yet.");
        ImGui::Spacing();
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("Use /w <user> <message> or right-click a user to start a 1:1 thread.");
        ImGui::PopTextWrapPos();
    } else {
        ImGui::Text("#%s has no messages yet.", data.current_room.c_str());
        ImGui::Spacing();
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("Tips: /join <room>  |  /w <user> <msg>  |  Ctrl+F find  |  Up/Down history  |  F12 Workbench");
        ImGui::PopTextWrapPos();
    }

    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

void GuiManager::render_timeline_line_context_menu(const ChatLogLine& item,
                                                   std::size_t line_idx,
                                                   bool is_self,
                                                   bool is_blocked) {
    if (!ImGui::BeginPopup("MsgCtx")) {
        return;
    }

    const auto& data = app_.get_data();
    const std::string ts = format_timestamp_hms(item.timestamp_ms);

    if (ImGui::MenuItem("Copy Message Text")) {
        ImGui::SetClipboardText(item.text.c_str());
    }
    if (ImGui::MenuItem("Copy Formatted Line")) {
        const std::string formatted = format_timeline_line_for_copy(item, ts);
        ImGui::SetClipboardText(formatted.c_str());
    }

    std::string ctx_peer;
    if (item.kind == ChatLineKind::CHAT && !is_self && !item.sender.empty() && item.sender != "(system)") {
        ctx_peer = item.sender;
    } else if (item.kind == ChatLineKind::WHISPER_IN && !is_self && !item.sender.empty()) {
        ctx_peer = item.sender;
    } else if (item.kind == ChatLineKind::WHISPER_OUT && !is_self && !item.recipient.empty()) {
        ctx_peer = item.recipient;
    }

    if (!ctx_peer.empty()) {
        ImGui::Separator();
        const std::string whisper_label = "Whisper @" + ctx_peer;
        if (ImGui::MenuItem(whisper_label.c_str())) {
            state_.active_whisper_target = ctx_peer;
            state_.focus_chat_input = true;
            state_.move_cursor_to_end = true;
        }
        const std::string thread_label = "Open 1:1 Whisper Thread (@" + ctx_peer + ")";
        if (ImGui::MenuItem(thread_label.c_str())) {
            state_.selected_whisper_peer = ctx_peer;
            state_.timeline_filter = TimelineFilter::WHISPER_PEER;
            state_.active_whisper_target = ctx_peer;
            state_.auto_scroll = true;
            state_.has_unread_below = false;
            state_.focus_chat_input = true;
            state_.move_cursor_to_end = true;
            app_.mark_whisper_peer_read(ctx_peer);
        }
        if (app_.is_user_blocked(ctx_peer)) {
            const std::string unblock_label = "Unblock @" + ctx_peer;
            if (ImGui::MenuItem(unblock_label.c_str(), nullptr, false, data.is_logged_in)) {
                app_.unblock_user(ctx_peer);
            }
        } else {
            const std::string block_label = "Block @" + ctx_peer;
            if (ImGui::MenuItem(block_label.c_str(), nullptr, false, data.is_logged_in)) {
                app_.block_user(ctx_peer);
            }
        }
    }

    if (is_blocked) {
        ImGui::Separator();
        const bool is_revealed = (state_.revealed_blocked_line_index == static_cast<int>(line_idx));
        if (ImGui::MenuItem(is_revealed ? "Collapse Message" : "Peek Message")) {
            state_.revealed_blocked_line_index = is_revealed ? -1 : static_cast<int>(line_idx);
        }
    }

    if (mode_ == UiMode::WORKBENCH && (item.message_id > 0 || !item.room_id.empty())) {
        ImGui::Separator();
        if (item.message_id > 0) {
            const std::string id_str = std::to_string(static_cast<unsigned long long>(item.message_id));
            const std::string copy_id_label = "Copy message_id (" + id_str + ")";
            if (ImGui::MenuItem(copy_id_label.c_str())) {
                ImGui::SetClipboardText(id_str.c_str());
            }
        }
        if (!item.room_id.empty()) {
            if (ImGui::MenuItem("Copy room_id")) {
                ImGui::SetClipboardText(item.room_id.c_str());
            }
        }
    }

    ImGui::EndPopup();
}

void GuiManager::render_chat_panel() {
    ImGui::Begin(
        "Chat",
        nullptr,
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    const auto& data = app_.get_data();
    const ImGuiStyle& style = ImGui::GetStyle();
    const float row_h = selectable_row_height();

    if (state_.timeline_filter == TimelineFilter::WHISPER_PEER &&
        !state_.selected_whisper_peer.empty() &&
        state_.auto_scroll &&
        app_.unread_whisper_count(state_.selected_whisper_peer) > 0) {
        app_.mark_whisper_peer_read(state_.selected_whisper_peer);
    } else if (state_.timeline_filter == TimelineFilter::WHISPERS &&
               state_.auto_scroll &&
               data.total_unread_whispers > 0) {
        app_.mark_all_whispers_read();
    }

    const std::string& self_user = data.session.effective_user;
    const std::string& self_display = data.session.display_name;
    const bool has_search = (state_.timeline_search_buffer[0] != '\0');
    std::vector<std::size_t> visible_indices;
    visible_indices.reserve(data.timeline.lines.size());
    for (std::size_t i = 0; i < data.timeline.lines.size(); ++i) {
        const auto& item = data.timeline.lines[i];
        const bool is_self = (!self_user.empty() && item.sender == self_user) ||
                             (!self_display.empty() && item.sender == self_display);
        const bool is_blocked = !is_self &&
                                (item.kind == ChatLineKind::CHAT || item.kind == ChatLineKind::WHISPER_IN) &&
                                !item.sender.empty() &&
                                app_.is_user_blocked(item.sender);
        if (state_.timeline_filter == TimelineFilter::WHISPERS) {
            if (item.kind != ChatLineKind::WHISPER_IN && item.kind != ChatLineKind::WHISPER_OUT) {
                continue;
            }
        } else if (state_.timeline_filter == TimelineFilter::WHISPER_PEER) {
            if (!matches_whisper_peer(item, state_.selected_whisper_peer)) {
                continue;
            }
        }
        if (is_blocked && state_.hide_blocked_messages) {
            continue;
        }
        if (has_search && !matches_timeline_search(item, state_.timeline_search_buffer)) {
            continue;
        }
        visible_indices.push_back(i);
    }

    // Dynamic Header
    ImGui::AlignTextToFramePadding();
    if (state_.timeline_filter == TimelineFilter::WHISPERS) {
        ImGui::TextUnformatted("All Whispers");
    } else if (state_.timeline_filter == TimelineFilter::WHISPER_PEER) {
        const std::string& peer = state_.selected_whisper_peer;
        ImGui::Text("@ %s", peer.empty() ? "unknown" : peer.c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("(1:1 Whisper)");
    } else {
        ImGui::Text("# %s", data.current_room.c_str());
        if (data.is_connected) {
            ImGui::SameLine();
            ImGui::TextDisabled("(%d online)", static_cast<int>(data.user_list.size()));
        }
    }

    const bool show_back = (state_.timeline_filter != TimelineFilter::ALL);
    const std::string back_label = "Back to #" + (data.current_room.empty() ? std::string("lobby") : data.current_room);
    const bool is_whispers_view = (state_.timeline_filter == TimelineFilter::WHISPERS);
    std::string whispers_label = "Whispers";
    if (data.total_unread_whispers > 0) {
        whispers_label += " (" + std::to_string(data.total_unread_whispers) + ")";
    }
    const std::string whispers_id = whispers_label + "###ChatWhispersToggle";
    const bool show_refresh = (mode_ == UiMode::WORKBENCH) && data.is_connected && data.is_logged_in;
    const bool show_leave = (state_.timeline_filter == TimelineFilter::ALL) &&
                            data.is_connected &&
                            data.is_logged_in &&
                            data.current_room != "lobby";

    const float back_w = show_back ? calc_button_width(back_label.c_str(), 4.0f) : 0.0f;
    const float whispers_w = calc_button_width(whispers_label.c_str(), 4.0f);
    const float find_w = calc_button_width("Find", 4.0f);
    const float refresh_w = show_refresh ? calc_button_width("Refresh Snapshot", 4.0f) : 0.0f;
    const float leave_w = show_leave ? calc_button_width("Leave Room", 4.0f) : 0.0f;

    float action_w = whispers_w + style.ItemSpacing.x + find_w;
    if (show_back) {
        action_w += back_w + style.ItemSpacing.x;
    }
    if (show_refresh) {
        action_w += refresh_w + style.ItemSpacing.x;
    }
    if (show_leave) {
        action_w += leave_w + style.ItemSpacing.x;
    }

    const float header_avail = ImGui::GetContentRegionAvail().x;
    if (header_avail > action_w) {
        ImGui::SameLine(ImGui::GetCursorPosX() + (header_avail - action_w));
    } else {
        ImGui::SameLine();
    }

    if (show_back) {
        if (ImGui::SmallButton(back_label.c_str())) {
            state_.timeline_filter = TimelineFilter::ALL;
            state_.selected_whisper_peer.clear();
            state_.auto_scroll = true;
            state_.has_unread_below = false;
        }
        ImGui::SameLine();
    }
    if (is_whispers_view) {
        push_accent_button_style();
    }
    if (ImGui::SmallButton(whispers_id.c_str())) {
        if (is_whispers_view) {
            state_.timeline_filter = TimelineFilter::ALL;
        } else {
            state_.timeline_filter = TimelineFilter::WHISPERS;
            state_.selected_whisper_peer.clear();
            app_.mark_all_whispers_read();
        }
        state_.auto_scroll = true;
        state_.has_unread_below = false;
    }
    if (is_whispers_view) {
        pop_accent_button_style();
    }
    ImGui::SameLine();
    if (state_.show_timeline_search) {
        push_accent_button_style();
    }
    if (ImGui::SmallButton("Find")) {
        state_.show_timeline_search = !state_.show_timeline_search;
        if (state_.show_timeline_search) {
            state_.focus_timeline_search = true;
        } else {
            state_.timeline_search_buffer[0] = '\0';
        }
    }
    if (state_.show_timeline_search) {
        pop_accent_button_style();
    }
    if (show_refresh) {
        ImGui::SameLine();
        if (ImGui::SmallButton("Refresh Snapshot")) {
            app_.request_refresh();
        }
    }
    if (show_leave) {
        ImGui::SameLine();
        if (ImGui::SmallButton("Leave Room")) {
            app_.leave_current_room();
        }
    }

    ImGui::Separator();

    if (state_.show_timeline_search) {
        char match_buf[32]{};
        std::snprintf(match_buf, sizeof(match_buf), "%zu matches", visible_indices.size());
        const float match_w = std::ceil(ImGui::CalcTextSize(match_buf).x + style.ItemSpacing.x);
        const float clear_w = has_search ? (calc_button_width("Clear", 8.0f) + style.ItemSpacing.x) : 0.0f;
        const float close_w = calc_button_width("Close", 8.0f) + style.ItemSpacing.x;
        const float search_w = std::max(100.0f, ImGui::GetContentRegionAvail().x - match_w - clear_w - close_w);

        if (state_.focus_timeline_search) {
            ImGui::SetKeyboardFocusHere();
            state_.focus_timeline_search = false;
        }
        ImGui::SetNextItemWidth(search_w);
        input_text_with_hint_ime(
            window_,
            "##TimelineSearch",
            "Search messages or @user... (Esc to close)",
            state_.timeline_search_buffer,
            sizeof(state_.timeline_search_buffer));
        if (ImGui::IsItemActive() &&
            !is_ime_composing(window_) &&
            ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            cancel_active_ime_composition(window_);
            state_.timeline_search_buffer[0] = '\0';
            state_.show_timeline_search = false;
        }
        ImGui::SameLine();
        ImGui::TextDisabled("%s", match_buf);
        if (has_search) {
            ImGui::SameLine();
            if (ImGui::Button("Clear", ImVec2(calc_button_width("Clear", 8.0f), 0.0f))) {
                state_.timeline_search_buffer[0] = '\0';
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Close", ImVec2(calc_button_width("Close", 8.0f), 0.0f))) {
            cancel_active_ime_composition(window_);
            state_.timeline_search_buffer[0] = '\0';
            state_.show_timeline_search = false;
        }
    }

    // Non-blocking Quick Connect card when disconnected
    if (!data.is_connected) {
        const float card_pad_x = std::round(14.0f * dpi_scale_);
        const float card_pad_y = std::round(12.0f * dpi_scale_);
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.129f, 0.149f, 0.200f, 1.00f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(card_pad_x, card_pad_y));
        ImGui::BeginChild(
            "QuickConnectCard",
            ImVec2(0.0f, 0.0f),
            ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding,
            ImGuiWindowFlags_NoScrollbar);

        ImGui::TextUnformatted("Not connected to a Dynaxis server.");
        ImGui::TextDisabled("Enter gateway endpoint and username below, or open Token / Options.");
        ImGui::Spacing();

        const float inner_w = std::max(260.0f, ImGui::GetContentRegionAvail().x);
        const float port_w = std::max(std::round(76.0f * dpi_scale_), std::floor(inner_w * 0.18f));
        const float host_w = std::max(std::round(120.0f * dpi_scale_), std::floor(inner_w * 0.44f));
        const float user_w = std::max(std::round(110.0f * dpi_scale_), inner_w - host_w - port_w - style.ItemSpacing.x * 2.0f);

        ImGui::SetNextItemWidth(host_w);
        const bool host_enter = input_text_with_hint_ime(
            window_,
            "##QuickHost",
            "Host (127.0.0.1)",
            state_.host_buffer,
            sizeof(state_.host_buffer),
            ImGuiInputTextFlags_EnterReturnsTrue);

        ImGui::SameLine();
        ImGui::SetNextItemWidth(port_w);
        ImGui::InputInt("##QuickPort", &state_.port, 0, 0);

        ImGui::SameLine();
        ImGui::SetNextItemWidth(user_w);
        const bool user_enter = input_text_with_hint_ime(
            window_,
            "##QuickUser",
            "Username",
            state_.username_buffer,
            sizeof(state_.username_buffer),
            ImGuiInputTextFlags_EnterReturnsTrue);

        ImGui::Spacing();
        push_accent_button_style();
        const float connect_w = calc_button_width("Connect", 14.0f);
        const bool connect_clicked = ImGui::Button("Connect", ImVec2(connect_w, 0.0f));
        pop_accent_button_style();
        if (connect_clicked || host_enter || user_enter) {
            cancel_active_ime_composition(window_);
            save_preferences();
            app_.reconnect_and_login(
                state_.host_buffer,
                state_.port,
                state_.username_buffer,
                state_.access_token_buffer,
                state_.resume_session);
        }
        if (app_.can_resume()) {
            ImGui::SameLine();
            const float resume_w = calc_button_width("Resume", 10.0f);
            if (ImGui::Button("Resume", ImVec2(resume_w, 0.0f))) {
                save_preferences();
                app_.reconnect_and_login(
                    state_.host_buffer,
                    state_.port,
                    state_.username_buffer,
                    state_.access_token_buffer,
                    true);
            }
        }
        ImGui::SameLine();
        const float opts_w = calc_button_width("Token / Options...", 8.0f);
        if (ImGui::Button("Token / Options...", ImVec2(opts_w, 0.0f))) {
            state_.show_connection_popup = true;
        }
        ImGui::SameLine();
        ImGui::PushStyleVar(
            ImGuiStyleVar_FramePadding,
            ImVec2(std::round(3.0f * dpi_scale_), std::round(3.0f * dpi_scale_)));
        bool quick_auto_resume = app_.auto_resume_enabled();
        if (ImGui::Checkbox("Auto-Resume", &quick_auto_resume)) {
            app_.set_auto_resume_enabled(quick_auto_resume);
            save_preferences();
        }
        ImGui::PopStyleVar();

        if (data.diagnostics.auto_resume_pending) {
            const std::uint64_t now_ms = current_unix_ms_wall();
            const std::uint64_t wait_ms =
                (data.diagnostics.auto_resume_next_unix_ms > now_ms)
                    ? (data.diagnostics.auto_resume_next_unix_ms - now_ms)
                    : 0ULL;
            const double wait_sec = static_cast<double>(wait_ms) / 1000.0;
            ImGui::Spacing();
            ImGui::AlignTextToFramePadding();
            ImGui::TextColored(
                ImVec4(0.96f, 0.72f, 0.30f, 1.0f),
                "Auto-resume attempt %u/%u in %.1fs...",
                data.diagnostics.auto_resume_attempt,
                kAutoResumeMaxAttempts,
                wait_sec);
            ImGui::SameLine();
            if (ImGui::SmallButton("Cancel")) {
                app_.cancel_auto_resume();
            }
        }

        ImGui::EndChild();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
    } else if (!data.diagnostics.history_available) {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.25f, 0.19f, 0.09f, 1.00f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(std::round(12.0f * dpi_scale_), std::round(8.0f * dpi_scale_)));
        ImGui::BeginChild(
            "PartialHistoryBanner",
            ImVec2(0.0f, 0.0f),
            ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding,
            ImGuiWindowFlags_NoScrollbar);
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(ImVec4(0.98f, 0.80f, 0.35f, 1.0f),
                           "Partial history only: recent DB window was unavailable (cache fallback).");
        ImGui::PopTextWrapPos();
        ImGui::EndChild();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
    }

    // Reserve footer space accurately from live font and style metrics (never overflows outer Chat window).
    std::vector<const SlashSuggestion*> matches;
    if (state_.chat_input[0] == '/') {
        const std::string typed(state_.chat_input);
        for (const auto& item : kSlashSuggestions) {
            if (item.admin_only && !data.is_admin) {
                continue;
            }
            const std::string cmd_str(item.command);
            if (cmd_str.rfind(typed, 0) == 0 || typed.rfind(cmd_str, 0) == 0) {
                matches.push_back(&item);
                if (matches.size() >= 5) {
                    break;
                }
            }
        }
    }

    const float suggest_pad_x = std::round(10.0f * dpi_scale_);
    const float suggest_pad_y = std::round(8.0f * dpi_scale_);
    float suggest_box_h = 0.0f;
    if (!matches.empty()) {
        suggest_box_h = suggest_pad_y * 2.0f +
                        ImGui::GetTextLineHeightWithSpacing() +
                        static_cast<float>(matches.size()) * row_h +
                        static_cast<float>(matches.size() - 1) * style.ItemSpacing.y;
    }

    float footer_height = style.ItemSpacing.y; // trailing gap after ScrollingRegion
    if (state_.has_unread_below) {
        footer_height += ImGui::GetTextLineHeight() + style.ItemSpacing.y;
    }
    if (!matches.empty()) {
        footer_height += suggest_box_h + style.ItemSpacing.y;
    }
    if (!state_.active_whisper_target.empty()) {
        footer_height += ImGui::GetFrameHeight() + style.ItemSpacing.y;
    }
    footer_height += style.SeparatorSize + style.ItemSpacing.y; // Separator
    footer_height += ImGui::GetFrameHeight() + style.ItemSpacing.y; // InputText + Send row
    footer_height += ImGui::GetTextLineHeight(); // Bottom hint + character counter

    const float scroll_h = std::max(1.0f, ImGui::GetContentRegionAvail().y - footer_height);

    const std::size_t current_line_count = data.timeline.lines.size();
    if (current_line_count > state_.last_seen_line_count && !state_.auto_scroll) {
        state_.has_unread_below = true;
    }
    state_.last_seen_line_count = current_line_count;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(std::round(10.0f * dpi_scale_), std::round(8.0f * dpi_scale_)));
    ImGui::BeginChild("ScrollingRegion", ImVec2(0.0f, scroll_h), ImGuiChildFlags_AlwaysUseWindowPadding);
    const float line_gap = std::round(2.0f * dpi_scale_);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(style.ItemSpacing.x, line_gap));

    const float avail_w = std::max(80.0f, ImGui::GetContentRegionAvail().x);
    const float gutter_w = std::clamp(
        std::ceil(ImGui::CalcTextSize("00:00:00").x + std::round(8.0f * dpi_scale_)),
        28.0f,
        std::max(28.0f, avail_w * 0.26f));

    if (visible_indices.empty()) {
        render_empty_timeline_card(has_search);
    } else {
        const float card_pad_x = std::round(8.0f * dpi_scale_);
        const float card_pad_v = std::max(1.0f, std::round(2.0f * dpi_scale_));
        const float chat_pad_x = std::round(4.0f * dpi_scale_);
        const float bar_w = std::max(3.0f, std::round(3.0f * dpi_scale_));
        const float card_rounding = std::round(4.0f * dpi_scale_);
        const float notice_gap = std::round(1.0f * dpi_scale_);
        const float block_gap = std::round(4.0f * dpi_scale_);

        ImDrawList* draw_list = ImGui::GetWindowDrawList();
        const ChatLogLine* prev_groupable = nullptr;
        ChatLineKind prev_visible_kind = ChatLineKind::CHAT;
        bool prev_was_collapsed = false;

        for (std::size_t pos = 0; pos < visible_indices.size(); ++pos) {
            const std::size_t line_idx = visible_indices[pos];
            const auto& item = data.timeline.lines[line_idx];
            const bool is_self = (!self_user.empty() && item.sender == self_user) ||
                                 (!self_display.empty() && item.sender == self_display);
            const bool is_blocked = !is_self &&
                                    (item.kind == ChatLineKind::CHAT || item.kind == ChatLineKind::WHISPER_IN) &&
                                    !item.sender.empty() &&
                                    app_.is_user_blocked(item.sender);
            const bool is_collapsed_blocked =
                is_blocked && !state_.hide_blocked_messages && (state_.revealed_blocked_line_index != static_cast<int>(line_idx));
            const bool is_grouped_chat =
                !is_blocked && !is_collapsed_blocked && prev_groupable != nullptr && can_group_chat_lines(*prev_groupable, item);

            if (pos > 0 && !is_grouped_chat) {
                const bool consecutive_sys =
                    !prev_was_collapsed &&
                    !is_collapsed_blocked &&
                    prev_visible_kind == ChatLineKind::SYSTEM_NOTICE &&
                    item.kind == ChatLineKind::SYSTEM_NOTICE;
                ImGui::Dummy(ImVec2(0.0f, consecutive_sys ? notice_gap : block_gap));
            }

            if (!is_blocked && !is_collapsed_blocked && item.kind == ChatLineKind::CHAT) {
                prev_groupable = &item;
            } else {
                prev_groupable = nullptr;
            }
            prev_visible_kind = item.kind;
            prev_was_collapsed = is_collapsed_blocked;

            const std::string ts = format_timestamp_hms(item.timestamp_ms);
            const bool is_whisper_or_collapsed =
                is_collapsed_blocked || item.kind == ChatLineKind::WHISPER_IN || item.kind == ChatLineKind::WHISPER_OUT;
            const bool is_inline_notice =
                !is_collapsed_blocked && (item.kind == ChatLineKind::SYSTEM_NOTICE || item.kind == ChatLineKind::ERROR_NOTICE);

            ImGui::PushID(static_cast<int>(line_idx));
            ImDrawListSplitter splitter;
            splitter.Split(draw_list, 2);
            splitter.SetCurrentChannel(draw_list, 1);

            const float start_x = ImGui::GetCursorPosX();
            const float row_left = ImGui::GetCursorScreenPos().x;
            const float row_right = row_left + avail_w;

            float hover_ts_y = 0.0f;
            bool has_hover_ts = false;

            ImGui::BeginGroup();
            if (is_collapsed_blocked) {
                ImGui::Dummy(ImVec2(0.0f, card_pad_v));
                ImGui::Indent(card_pad_x);
                ImGui::PushTextWrapPos(std::max(start_x + card_pad_x + 40.0f, start_x + avail_w - card_pad_x));
                ImGui::TextDisabled("[Blocked message from @%s - click to peek, right-click to unblock]", item.sender.c_str());
                ImGui::PopTextWrapPos();
                ImGui::Unindent(card_pad_x);
                ImGui::Dummy(ImVec2(0.0f, card_pad_v));
            } else if (item.kind == ChatLineKind::CHAT) {
                const float indent_x = chat_pad_x + gutter_w;
                if (!is_grouped_chat) {
                    ImGui::SetCursorPosX(start_x + chat_pad_x);
                    ImGui::TextDisabled("%s", ts.c_str());
                    ImGui::SameLine(start_x + indent_x);
                    const ImVec4 sender_color = is_self
                                                    ? ImVec4(0.42f, 0.76f, 0.98f, 1.0f)
                                                    : ImVec4(0.92f, 0.80f, 0.48f, 1.0f);
                    ImGui::TextColored(sender_color, "%s", item.sender.empty() ? "unknown" : item.sender.c_str());
                    if (is_self) {
                        ImGui::SameLine();
                        ImGui::TextDisabled("[You]");
                    }
                    if (!item.room_name.empty() && item.room_name != data.current_room) {
                        ImGui::SameLine();
                        ImGui::TextDisabled("[#%s]", item.room_name.c_str());
                    }
                } else {
                    hover_ts_y = ImGui::GetCursorScreenPos().y;
                    has_hover_ts = true;
                }
                ImGui::Indent(indent_x);
                ImGui::PushTextWrapPos(0.0f);
                ImGui::TextUnformatted(item.text.c_str());
                ImGui::PopTextWrapPos();
                ImGui::Unindent(indent_x);
            } else if (is_inline_notice) {
                const float indent_x = chat_pad_x + gutter_w;
                const bool is_err = (item.kind == ChatLineKind::ERROR_NOTICE);
                const ImVec4 tag_color = is_err ? ImVec4(0.96f, 0.44f, 0.46f, 1.0f) : ImVec4(0.56f, 0.70f, 0.90f, 1.0f);
                const ImVec4 body_color = is_err ? ImVec4(0.98f, 0.72f, 0.74f, 1.0f) : ImVec4(0.76f, 0.81f, 0.90f, 1.0f);
                const char* tag = is_err ? "[ERR]" : "[SYS]";

                ImGui::SetCursorPosX(start_x + chat_pad_x);
                ImGui::TextDisabled("%s", ts.c_str());
                ImGui::SameLine(start_x + indent_x);
                ImGui::TextColored(tag_color, "%s", tag);
                ImGui::SameLine(0.0f, std::round(6.0f * dpi_scale_));
                const float msg_x = ImGui::GetCursorPosX();
                const float line_y = ImGui::GetCursorPosY();
                ImGui::Indent(indent_x);
                ImGui::SetCursorPos(ImVec2(msg_x, line_y));
                ImGui::PushTextWrapPos(std::max(start_x + indent_x + 40.0f, start_x + avail_w - chat_pad_x));
                ImGui::TextColored(body_color, "%s", item.text.c_str());
                ImGui::PopTextWrapPos();
                ImGui::Unindent(indent_x);
            } else {
                const float indent_x = card_pad_x + gutter_w;
                ImGui::Dummy(ImVec2(0.0f, card_pad_v));
                ImGui::SetCursorPosX(start_x + card_pad_x);
                ImGui::TextDisabled("%s", ts.c_str());
                ImGui::SameLine(start_x + indent_x);
                if (item.kind == ChatLineKind::WHISPER_IN) {
                    ImGui::TextColored(ImVec4(0.78f, 0.60f, 1.00f, 1.0f), "DM <- @%s", item.sender.c_str());
                } else {
                    ImGui::TextColored(ImVec4(0.46f, 0.86f, 0.84f, 1.0f), "DM -> @%s", item.recipient.c_str());
                }
                ImGui::Indent(indent_x);
                ImGui::PushTextWrapPos(std::max(start_x + indent_x + 40.0f, start_x + avail_w - card_pad_x));
                ImGui::TextUnformatted(item.text.c_str());
                ImGui::PopTextWrapPos();
                ImGui::Unindent(indent_x);
                ImGui::Dummy(ImVec2(0.0f, card_pad_v));
            }
            ImGui::EndGroup();

            const ImVec2 grp_min = ImGui::GetItemRectMin();
            const ImVec2 grp_max = ImGui::GetItemRectMax();
            const float rect_pad_y = is_inline_notice ? std::round(1.0f * dpi_scale_) : 0.0f;
            const ImVec2 card_min(row_left, grp_min.y - rect_pad_y);
            const ImVec2 card_max(row_right, grp_max.y + rect_pad_y);
            const bool row_hovered =
                ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(card_min, card_max, true);

            splitter.SetCurrentChannel(draw_list, 0);
            if (is_whisper_or_collapsed || is_inline_notice) {
                ImU32 bg_col = IM_COL32(34, 38, 48, 175);
                ImU32 bar_col = 0;
                if (is_collapsed_blocked) {
                    bg_col = IM_COL32(28, 30, 38, 180);
                } else if (item.kind == ChatLineKind::WHISPER_IN) {
                    bg_col = IM_COL32(52, 38, 72, 165);
                    bar_col = IM_COL32(182, 138, 252, 235);
                } else if (item.kind == ChatLineKind::WHISPER_OUT) {
                    bg_col = IM_COL32(30, 56, 62, 165);
                    bar_col = IM_COL32(108, 214, 208, 235);
                } else if (item.kind == ChatLineKind::SYSTEM_NOTICE) {
                    bg_col = IM_COL32(30, 36, 48, 80);
                } else if (item.kind == ChatLineKind::ERROR_NOTICE) {
                    bg_col = IM_COL32(68, 30, 34, 175);
                    bar_col = IM_COL32(236, 98, 102, 240);
                }
                draw_list->AddRectFilled(card_min, card_max, bg_col, card_rounding);
                if (bar_col != 0) {
                    draw_list->AddRectFilled(
                        card_min,
                        ImVec2(card_min.x + bar_w, card_max.y),
                        bar_col,
                        card_rounding,
                        ImDrawFlags_RoundCornersLeft);
                }
                if (row_hovered) {
                    draw_list->AddRectFilled(card_min, card_max, IM_COL32(255, 255, 255, 14), card_rounding);
                }
            } else if (row_hovered) {
                draw_list->AddRectFilled(card_min, card_max, IM_COL32(255, 255, 255, 12), card_rounding);
            }

            if (is_grouped_chat && has_hover_ts && row_hovered) {
                draw_list->AddText(
                    ImVec2(row_left + chat_pad_x, hover_ts_y),
                    ImGui::GetColorU32(ImGuiCol_TextDisabled),
                    ts.c_str());
            }

            splitter.Merge(draw_list);

            if (row_hovered && is_collapsed_blocked && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                state_.revealed_blocked_line_index = static_cast<int>(line_idx);
            }
            if (row_hovered && ImGui::IsMouseReleased(ImGuiMouseButton_Right)) {
                ImGui::OpenPopup("MsgCtx");
            }
            render_timeline_line_context_menu(item, line_idx, is_self, is_blocked);

            ImGui::PopID();
        }
    }

    const float max_scroll = ImGui::GetScrollMaxY();
    const float current_scroll = ImGui::GetScrollY();
    if (max_scroll > 0.0f && current_scroll < max_scroll - 10.0f) {
        state_.auto_scroll = false;
    } else if (current_scroll >= max_scroll - 4.0f) {
        state_.auto_scroll = true;
        state_.has_unread_below = false;
    }

    if (state_.auto_scroll) {
        ImGui::SetScrollHereY(1.0f);
        state_.has_unread_below = false;
    }
    ImGui::PopStyleVar();
    ImGui::EndChild();
    ImGui::PopStyleVar();

    if (state_.has_unread_below) {
        if (ImGui::SmallButton("Jump to latest messages")) {
            state_.auto_scroll = true;
            state_.has_unread_below = false;
        }
    }

    if (!matches.empty()) {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.118f, 0.129f, 0.165f, 1.00f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(suggest_pad_x, suggest_pad_y));
        ImGui::PushStyleVar(ImGuiStyleVar_SelectableTextAlign, ImVec2(0.0f, 0.5f));
        ImGui::BeginChild(
            "SlashSuggestBox",
            ImVec2(0.0f, suggest_box_h),
            ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding,
            ImGuiWindowFlags_NoScrollbar);
        ImGui::TextDisabled("Slash commands (Tab or click to insert)");
        for (const auto* s : matches) {
            const std::string line = std::string(s->syntax) + "  -  " + s->description;
            if (ImGui::Selectable(line.c_str(), false, ImGuiSelectableFlags_None, ImVec2(0.0f, row_h))) {
                std::snprintf(state_.chat_input, sizeof(state_.chat_input), "%s", s->command);
                state_.pending_chat_input = s->command;
                state_.move_cursor_to_end = true;
                state_.focus_chat_input = true;
            }
        }
        ImGui::EndChild();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor();
    }

    if (!state_.active_whisper_target.empty()) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(ImVec4(0.48f, 0.84f, 0.82f, 1.0f), "Whispering to @%s", state_.active_whisper_target.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("Clear Whisper Target")) {
            state_.active_whisper_target.clear();
        }
    }

    ImGui::Separator();

    const float send_btn_width = calc_button_width("Send", 14.0f);
    ImGui::SetNextItemWidth(std::max(100.0f, ImGui::GetContentRegionAvail().x - send_btn_width - style.ItemSpacing.x));
    if (state_.focus_chat_input) {
        if (!state_.pending_chat_input.empty()) {
            std::snprintf(state_.chat_input, sizeof(state_.chat_input), "%s", state_.pending_chat_input.c_str());
        }
        ImGui::SetKeyboardFocusHere();
        state_.focus_chat_input = false;
    }

    const char* input_hint = state_.active_whisper_target.empty()
                                 ? "Type a message or /command..."
                                 : "Type a private whisper (or /command)...";
    const ImGuiInputTextFlags input_flags = ImGuiInputTextFlags_EnterReturnsTrue |
                                            ImGuiInputTextFlags_CallbackCompletion |
                                            ImGuiInputTextFlags_CallbackHistory |
                                            ImGuiInputTextFlags_CallbackAlways;
    const bool enter_submit = input_text_with_hint_ime(
        window_,
        "##Input",
        input_hint,
        state_.chat_input,
        sizeof(state_.chat_input),
        input_flags,
        &state_,
        data.is_admin);

    ImGui::SameLine();
    push_accent_button_style();
    const bool send_clicked = ImGui::Button("Send", ImVec2(send_btn_width, 0.0f));
    pop_accent_button_style();

    if ((enter_submit || send_clicked) && state_.chat_input[0] != '\0') {
        if (data.is_connected && data.is_logged_in) {
            cancel_active_ime_composition(window_);
            const std::string text(state_.chat_input);
            state_.input_history.record(text);
            if (!state_.active_whisper_target.empty() && text[0] != '/') {
                app_.whisper(state_.active_whisper_target, text);
            } else {
                app_.process_command(text);
            }
            state_.chat_input[0] = '\0';
            state_.auto_scroll = true;
            if (send_clicked) {
                state_.focus_chat_input = true;
            }
        }
    }

    const Utf8CharStats char_stats = measure_utf8_char_stats(state_.chat_input);
    const std::string total_str = format_with_commas(char_stats.total_codepoints);
    const std::string units_str = format_with_commas(char_stats.weighted_units);
    const std::string max_units_str = format_with_commas(kMaxChatCharUnits);
    char count_buf[96]{};
    if (char_stats.unicode_codepoints > 0) {
        std::snprintf(
            count_buf,
            sizeof(count_buf),
            "%s chars (%s / %s)",
            total_str.c_str(),
            units_str.c_str(),
            max_units_str.c_str());
    } else {
        std::snprintf(
            count_buf,
            sizeof(count_buf),
            "%s / %s chars",
            total_str.c_str(),
            max_units_str.c_str());
    }
    const ImVec2 count_sz = ImGui::CalcTextSize(count_buf);
    const float hint_avail = ImGui::GetContentRegionAvail().x;
    const char* full_hint = data.is_admin
                                ? "Commands: /join /w /invite /kick /block /unblock /mute /ban /gkick"
                                : "Commands: /join /w /invite /kick /block /unblock /blacklist list";
    const ImVec2 full_hint_sz = ImGui::CalcTextSize(full_hint);
    const bool show_full_hint = (hint_avail > full_hint_sz.x + count_sz.x + style.ItemSpacing.x * 2.0f);
    ImGui::TextDisabled("%s", show_full_hint ? full_hint : "Up/Down history | Ctrl+F find | F12 workbench");

    ImVec4 count_color = style.Colors[ImGuiCol_TextDisabled];
    if (char_stats.weighted_units >= 1980) {
        count_color = ImVec4(0.95f, 0.42f, 0.44f, 1.0f);
    } else if (char_stats.weighted_units >= 1800) {
        count_color = ImVec4(0.96f, 0.72f, 0.30f, 1.0f);
    }
    if (hint_avail > count_sz.x + std::round(120.0f * dpi_scale_)) {
        ImGui::SameLine(ImGui::GetCursorPosX() + (hint_avail - count_sz.x));
    } else {
        ImGui::SameLine();
    }
    ImGui::TextColored(count_color, "%s", count_buf);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "Limit: 1,000 Unicode / 2,000 Latin chars\nCurrent: %zu chars (Unicode %zu, Latin/ASCII %zu)",
            char_stats.total_codepoints,
            char_stats.unicode_codepoints,
            char_stats.latin_codepoints);
    }

    ImGui::End();
}

void GuiManager::render_participants_panel() {
    ImGui::Begin(
        "Participants",
        nullptr,
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    const auto& data = app_.get_data();
    const ImGuiStyle& style = ImGui::GetStyle();
    const float row_h = selectable_row_height();
    const ImVec2 list_pad(std::round(10.0f * dpi_scale_), std::round(8.0f * dpi_scale_));

    if (data.is_connected) {
        const auto issue_command = [this, &data](const std::string& cmd) {
            if (data.is_logged_in) {
                app_.process_command(cmd);
            }
        };

        ImGui::AlignTextToFramePadding();
        ImGui::Text("Online (%d)", static_cast<int>(data.user_list.size()));
        const float sync_w = calc_button_width("Sync Blocklist", 6.0f);
        const float avail = ImGui::GetContentRegionAvail().x;
        if (avail > sync_w) {
            ImGui::SameLine(ImGui::GetCursorPosX() + (avail - sync_w));
        } else {
            ImGui::SameLine();
        }
        if (ImGui::SmallButton("Sync Blocklist")) {
            app_.request_blacklist();
        }
        ImGui::Separator();

        const ImVec2 small_pad(std::round(2.0f * dpi_scale_), std::round(2.0f * dpi_scale_));
        float blocked_list_h = 0.0f;
        float blocked_footer_h = 0.0f;
        if (!data.blocked_users.empty()) {
            const int visible_blocked = std::min(3, static_cast<int>(data.blocked_users.size()));
            blocked_list_h = list_pad.y * 2.0f +
                             static_cast<float>(visible_blocked) * ImGui::GetFrameHeight() +
                             static_cast<float>(std::max(0, visible_blocked - 1)) * style.ItemSpacing.y;
            const float compact_header_h = ImGui::GetTextLineHeight() + small_pad.y * 2.0f;
            blocked_footer_h = style.ItemSpacing.y +
                               (style.SeparatorSize + style.ItemSpacing.y) +
                               (compact_header_h + style.ItemSpacing.y) +
                               blocked_list_h;
        }

        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, list_pad);
        ImGui::BeginChild("UserListRegion", ImVec2(0.0f, -blocked_footer_h), ImGuiChildFlags_AlwaysUseWindowPadding);
        ImGui::PushStyleVar(ImGuiStyleVar_SelectableTextAlign, ImVec2(0.0f, 0.5f));

        for (const auto& u : data.user_list) {
            const bool is_self = (!data.session.effective_user.empty() && u == data.session.effective_user) ||
                                 (!data.session.display_name.empty() && u == data.session.display_name);
            const bool is_blocked = app_.is_user_blocked(u);

            std::string label = u;
            if (is_self) {
                label += "  [You]";
            } else if (is_blocked) {
                label += "  [Blocked]";
            }

            ImGui::Selectable(label.c_str(), false, ImGuiSelectableFlags_None, ImVec2(0.0f, row_h));
            if (!is_self && ImGui::BeginPopupContextItem()) {
                if (ImGui::MenuItem("Whisper")) {
                    state_.active_whisper_target = u;
                    state_.focus_chat_input = true;
                    state_.move_cursor_to_end = true;
                }
                if (ImGui::MenuItem("Open 1:1 Whisper Thread")) {
                    state_.selected_whisper_peer = u;
                    state_.timeline_filter = TimelineFilter::WHISPER_PEER;
                    state_.active_whisper_target = u;
                    state_.auto_scroll = true;
                    state_.has_unread_below = false;
                    state_.focus_chat_input = true;
                    state_.move_cursor_to_end = true;
                    app_.mark_whisper_peer_read(u);
                }
                const bool room_action_enabled = (data.current_room != "lobby");
                if (ImGui::MenuItem("Invite to Current Room", nullptr, false, room_action_enabled)) {
                    issue_command("/invite " + u + " " + data.current_room);
                }
                if (ImGui::MenuItem("Kick from Current Room", nullptr, false, room_action_enabled)) {
                    issue_command("/kick " + u + " " + data.current_room);
                }
                ImGui::Separator();
                if (is_blocked) {
                    if (ImGui::MenuItem("Unblock")) {
                        app_.unblock_user(u);
                    }
                } else {
                    if (ImGui::MenuItem("Block")) {
                        app_.block_user(u);
                    }
                }

                if (data.is_admin) {
                    ImGui::Separator();
                    if (ImGui::BeginMenu("Admin Mute")) {
                        if (ImGui::MenuItem("30s")) issue_command("/mute " + u + " 30");
                        if (ImGui::MenuItem("5m")) issue_command("/mute " + u + " 300");
                        if (ImGui::MenuItem("30m")) issue_command("/mute " + u + " 1800");
                        if (ImGui::MenuItem("Unmute")) issue_command("/unmute " + u);
                        ImGui::EndMenu();
                    }
                    if (ImGui::BeginMenu("Admin Ban")) {
                        if (ImGui::MenuItem("10m")) issue_command("/ban " + u + " 600");
                        if (ImGui::MenuItem("1h")) issue_command("/ban " + u + " 3600");
                        if (ImGui::MenuItem("24h")) issue_command("/ban " + u + " 86400");
                        if (ImGui::MenuItem("Unban")) issue_command("/unban " + u);
                        ImGui::EndMenu();
                    }
                    if (ImGui::MenuItem("Global Kick (/gkick)")) {
                        issue_command("/gkick " + u);
                    }
                }
                ImGui::EndPopup();
            }
        }
        ImGui::PopStyleVar();
        ImGui::EndChild();
        ImGui::PopStyleVar();

        if (!data.blocked_users.empty()) {
            ImGui::Separator();
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, small_pad);
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("BLOCKED (%d)", static_cast<int>(data.blocked_users.size()));
            const float chk_w = ImGui::GetFrameHeight() + style.ItemInnerSpacing.x + ImGui::CalcTextSize("Hide in chat").x;
            const float hdr_avail = ImGui::GetContentRegionAvail().x;
            if (hdr_avail > chk_w) {
                ImGui::SameLine(ImGui::GetCursorPosX() + (hdr_avail - chk_w));
            } else {
                ImGui::SameLine();
            }
            ImGui::Checkbox("Hide in chat", &state_.hide_blocked_messages);
            ImGui::PopStyleVar();

            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, list_pad);
            ImGui::BeginChild("BlockedUsersRegion", ImVec2(0.0f, blocked_list_h), ImGuiChildFlags_AlwaysUseWindowPadding);
            for (const auto& blocked : data.blocked_users) {
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted(blocked.c_str());
                const std::string btn_id = "Unblock##" + blocked;
                const float btn_w = calc_button_width("Unblock", 4.0f);
                const float row_avail = ImGui::GetContentRegionAvail().x;
                if (row_avail > btn_w) {
                    ImGui::SameLine(ImGui::GetCursorPosX() + (row_avail - btn_w));
                } else {
                    ImGui::SameLine();
                }
                if (ImGui::SmallButton(btn_id.c_str())) {
                    app_.unblock_user(blocked);
                }
            }
            ImGui::EndChild();
            ImGui::PopStyleVar();
        }
    } else {
        ImGui::TextDisabled("Not connected");
    }

    ImGui::End();
}

void GuiManager::render_diagnostics_panel() {
    ImGui::Begin(
        "Diagnostics",
        nullptr,
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    const auto& data = app_.get_data();
    const ImGuiStyle& style = ImGui::GetStyle();
    const ImVec2 small_pad(std::round(2.0f * dpi_scale_), std::round(2.0f * dpi_scale_));
    const std::uint64_t now_ms = current_unix_ms_wall();

    const auto render_kv_row = [](const char* key, const std::string& val, const ImVec4* val_color = nullptr) {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextDisabled("%s", key);
        ImGui::TableSetColumnIndex(1);
        ImGui::PushTextWrapPos(0.0f);
        if (val_color) {
            ImGui::TextColored(*val_color, "%s", val.c_str());
        } else {
            ImGui::TextUnformatted(val.empty() ? "-" : val.c_str());
        }
        ImGui::PopTextWrapPos();
    };

    const double now_time = ImGui::GetTime();
    const float stable_copy_w = calc_button_width("Copied", 4.0f);
    const auto render_copyable_kv_row = [&](const char* feedback_key, const char* key, const std::string& val) {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", key);
        ImGui::TableSetColumnIndex(1);
        if (val.empty()) {
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted("-");
            return;
        }
        const bool is_copied =
            (state_.copied_feedback_key == feedback_key) && (now_time < state_.copied_feedback_until);
        const char* btn_label = is_copied ? "Copied" : "Copy";
        const float cell_start_x = ImGui::GetCursorPosX();
        const float avail = ImGui::GetContentRegionAvail().x;
        const float text_w = std::max(40.0f, avail - stable_copy_w - ImGui::GetStyle().ItemSpacing.x);
        ImGui::PushTextWrapPos(cell_start_x + text_w);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(val.c_str());
        ImGui::PopTextWrapPos();
        if (avail > stable_copy_w + 16.0f) {
            ImGui::SameLine(cell_start_x + (avail - stable_copy_w));
        } else {
            ImGui::SameLine();
        }
        ImGui::PushID(feedback_key);
        if (ImGui::SmallButton(btn_label)) {
            ImGui::SetClipboardText(val.c_str());
            state_.copied_feedback_key = feedback_key;
            state_.copied_feedback_until = now_time + 1.5;
        }
        ImGui::PopID();
    };

    const ImGuiTableFlags kv_flags = ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_PadOuterX;

    if (ImGui::BeginTabBar("DiagnosticsTabBar")) {
        if (ImGui::BeginTabItem("Inspector")) {
            ImGui::BeginChild("DiagInspectorScroll", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None);

            if (ImGui::CollapsingHeader("Connection & Latency", ImGuiTreeNodeFlags_DefaultOpen)) {
                if (ImGui::BeginTable("DiagConnTable", 2, kv_flags)) {
                    ImGui::TableSetupColumn("Key", ImGuiTableColumnFlags_WidthStretch, 0.42f);
                    ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch, 0.58f);
                    const ImVec4 state_col = transport_state_color(data.connection_state);
                    render_kv_row("State", transport_state_label(data.connection_state), &state_col);
                    render_kv_row("Endpoint", data.last_host.empty() ? "-" : (data.last_host + ":" + std::to_string(data.last_port)));
                    char caps_buf[24]{};
                    std::snprintf(caps_buf, sizeof(caps_buf), "0x%04X", static_cast<unsigned>(data.diagnostics.server_caps));
                    render_kv_row("Server Caps", caps_buf);
                    if (data.diagnostics.has_rtt) {
                        render_kv_row("Last RTT", std::to_string(data.diagnostics.last_rtt_ms) + " ms");
                        char rtt_summary[64]{};
                        std::snprintf(
                            rtt_summary,
                            sizeof(rtt_summary),
                            "%u / %u / %u ms (%u)",
                            data.diagnostics.min_rtt_ms,
                            data.diagnostics.avg_rtt_ms,
                            data.diagnostics.max_rtt_ms,
                            data.diagnostics.rtt_sample_count);
                        render_kv_row("Min/Avg/Max", rtt_summary);
                    } else {
                        render_kv_row("Last RTT", "pending");
                    }
                    ImGui::EndTable();
                }

                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, small_pad);
                bool auto_ping = app_.auto_ping_enabled();
                if (ImGui::Checkbox("Auto Ping (10s)", &auto_ping)) {
                    app_.set_auto_ping_enabled(auto_ping);
                    save_preferences();
                }
                ImGui::PopStyleVar();
                if (data.is_connected) {
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Send Ping")) {
                        app_.request_ping();
                    }
                }
                ImGui::Spacing();
            }

            if (ImGui::CollapsingHeader("Identity & Continuity", ImGuiTreeNodeFlags_DefaultOpen)) {
                if (ImGui::BeginTable("DiagIdentityTable", 2, kv_flags)) {
                    ImGui::TableSetupColumn("Key", ImGuiTableColumnFlags_WidthStretch, 0.42f);
                    ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch, 0.58f);
                    render_kv_row("Display Name", data.session.display_name);
                    render_kv_row("Effective User", data.session.effective_user);
                    render_copyable_kv_row("principal_id", "Principal ID", data.session.principal_id);
                    render_copyable_kv_row("auth_session_id", "Auth Session", data.session.auth_session_id);
                    render_copyable_kv_row("logical_session_id", "Logical Session", data.session.logical_session_id);
                    render_kv_row("Resumed", data.session.resumed ? "Yes" : "No");
                    render_kv_row("Resume Token", data.session.has_resume_token ? "Yes (redacted)" : "No");
                    if (data.session.resume_expires_unix_ms > 0) {
                        const std::string ttl = format_remaining_ttl(data.session.resume_expires_unix_ms, now_ms);
                        ImVec4 ttl_col = ImVec4(0.24f, 0.78f, 0.52f, 1.0f);
                        if (now_ms >= data.session.resume_expires_unix_ms) {
                            ttl_col = ImVec4(0.95f, 0.42f, 0.44f, 1.0f);
                        } else if ((data.session.resume_expires_unix_ms - now_ms) < 60'000ULL) {
                            ttl_col = ImVec4(0.96f, 0.72f, 0.30f, 1.0f);
                        }
                        render_kv_row("Resume TTL", ttl, &ttl_col);
                    }
                    ImGui::EndTable();
                }
                ImGui::Spacing();
            }

            if (ImGui::CollapsingHeader("Snapshot & Room Generation", ImGuiTreeNodeFlags_DefaultOpen)) {
                if (ImGui::BeginTable("DiagSnapshotTable", 2, kv_flags)) {
                    ImGui::TableSetupColumn("Key", ImGuiTableColumnFlags_WidthStretch, 0.42f);
                    ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch, 0.58f);
                    render_kv_row("Current Room", data.current_room);
                    render_copyable_kv_row("room_id", "Room UUID", data.diagnostics.current_room_id);
                    render_kv_row("Watermark", std::to_string(static_cast<unsigned long long>(data.diagnostics.current_watermark)));
                    render_kv_row("History Ready", data.diagnostics.history_available ? "true" : "false");
                    ImGui::EndTable();
                }
                if (data.is_connected && data.is_logged_in) {
                    ImGui::Spacing();
                    if (ImGui::SmallButton("Refresh Snapshot")) {
                        app_.request_refresh();
                    }
                }
                ImGui::Spacing();
            }

            if (ImGui::CollapsingHeader("Continuity Controls & Errors", ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, small_pad);
                bool auto_resume = app_.auto_resume_enabled();
                if (ImGui::Checkbox("Auto-Resume (Exp. Backoff)", &auto_resume)) {
                    app_.set_auto_resume_enabled(auto_resume);
                    save_preferences();
                }
                ImGui::PopStyleVar();

                if (data.diagnostics.auto_resume_pending) {
                    const std::uint64_t wait_ms =
                        (data.diagnostics.auto_resume_next_unix_ms > now_ms)
                            ? (data.diagnostics.auto_resume_next_unix_ms - now_ms)
                            : 0ULL;
                    ImGui::TextColored(
                        ImVec4(0.96f, 0.72f, 0.30f, 1.0f),
                        "Pending retry %u/%u (%.1fs)",
                        data.diagnostics.auto_resume_attempt,
                        kAutoResumeMaxAttempts,
                        static_cast<double>(wait_ms) / 1000.0);
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Cancel Retry")) {
                        app_.cancel_auto_resume();
                    }
                }

                ImGui::Spacing();
                if (data.is_connected && ImGui::Button("Drop Socket")) {
                    app_.disconnect();
                }
                if (app_.can_resume()) {
                    if (data.is_connected) {
                        ImGui::SameLine();
                    }
                    if (ImGui::Button("Reconnect + Resume")) {
                        app_.reconnect_and_login(
                            state_.host_buffer,
                            state_.port,
                            state_.username_buffer,
                            state_.access_token_buffer,
                            true);
                    }
                }
                if (ImGui::Button("Reconnect Fresh")) {
                    app_.reconnect_and_login(
                        state_.host_buffer,
                        state_.port,
                        state_.username_buffer,
                        state_.access_token_buffer,
                        false);
                }
                if (app_.can_resume()) {
                    ImGui::SameLine();
                    if (ImGui::Button("Clear Resume Token")) {
                        app_.clear_saved_resume_token();
                    }
                }
                if (data.diagnostics.last_error_code != 0 || !data.diagnostics.last_error_message.empty()) {
                    ImGui::Spacing();
                    ImGui::PushTextWrapPos(0.0f);
                    ImGui::TextColored(
                        ImVec4(0.95f, 0.42f, 0.44f, 1.0f),
                        "Last Error (%u): %s",
                        static_cast<unsigned>(data.diagnostics.last_error_code),
                        data.diagnostics.last_error_message.c_str());
                    ImGui::PopTextWrapPos();
                }
                if (!data.diagnostics.last_disconnect_reason.empty()) {
                    ImGui::Spacing();
                    ImGui::PushTextWrapPos(0.0f);
                    ImGui::TextDisabled("Last Disconnect: %s", data.diagnostics.last_disconnect_reason.c_str());
                    ImGui::PopTextWrapPos();
                }
            }

            ImGui::EndChild();
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Protocol Log")) {
            const float copy_w = calc_button_width("Copy", 4.0f);
            const float clear_w = calc_button_width("Clear", 4.0f);
            const float filter_w = std::max(80.0f, ImGui::GetContentRegionAvail().x - copy_w - clear_w - style.ItemSpacing.x * 2.0f);
            ImGui::SetNextItemWidth(filter_w);
            input_text_with_hint_ime(
                window_,
                "##ProtocolFilter",
                "Filter opcode or summary...",
                state_.protocol_filter_buffer,
                sizeof(state_.protocol_filter_buffer));

            std::vector<const ProtocolEventEntry*> visible_events;
            visible_events.reserve(data.protocol_events.size());
            for (const auto& ev : data.protocol_events) {
                if (state_.hide_ping_pong_logs && (ev.opcode == "MSG_PING" || ev.opcode == "MSG_PONG")) {
                    continue;
                }
                if (state_.protocol_filter_buffer[0] != '\0' &&
                    !contains_case_insensitive(ev.opcode, state_.protocol_filter_buffer) &&
                    !contains_case_insensitive(ev.summary, state_.protocol_filter_buffer)) {
                    continue;
                }
                visible_events.push_back(&ev);
            }

            ImGui::SameLine();
            if (ImGui::SmallButton("Copy")) {
                std::string dump;
                for (const auto* ev : visible_events) {
                    dump += "[" + format_timestamp_hms(ev->timestamp_ms) + "] " +
                            protocol_direction_label(ev->direction) + " " +
                            ev->opcode + " | " + ev->summary + "\n";
                }
                ImGui::SetClipboardText(dump.c_str());
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Clear")) {
                app_.clear_protocol_events();
            }

            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, small_pad);
            ImGui::Checkbox("Hide PING/PONG", &state_.hide_ping_pong_logs);
            ImGui::SameLine();
            ImGui::TextDisabled("(%zu / %zu events)", visible_events.size(), data.protocol_events.size());
            ImGui::PopStyleVar();

            const ImVec2 log_pad(std::round(8.0f * dpi_scale_), std::round(6.0f * dpi_scale_));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, log_pad);
            ImGui::BeginChild(
                "ProtocolLogRegion",
                ImVec2(0.0f, 0.0f),
                ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding);
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(style.ItemSpacing.x, std::round(2.0f * dpi_scale_)));

            if (visible_events.empty()) {
                ImGui::TextDisabled("No protocol events recorded.");
            } else {
                for (const auto* ev : visible_events) {
                    const std::string ts = format_timestamp_hms(ev->timestamp_ms);
                    ImGui::TextDisabled("[%s]", ts.c_str());
                    ImGui::SameLine();
                    ImGui::TextColored(protocol_direction_color(ev->direction), "%s", protocol_direction_label(ev->direction));
                    ImGui::SameLine();
                    ImGui::TextUnformatted(ev->opcode.c_str());
                    if (!ev->summary.empty()) {
                        ImGui::SameLine();
                        ImGui::PushTextWrapPos(0.0f);
                        ImGui::TextDisabled("%s", ev->summary.c_str());
                        ImGui::PopTextWrapPos();
                    }
                }
            }

            const float max_scroll = ImGui::GetScrollMaxY();
            const float cur_scroll = ImGui::GetScrollY();
            if (max_scroll > 0.0f && cur_scroll < max_scroll - 10.0f) {
                state_.protocol_auto_scroll = false;
            } else if (cur_scroll >= max_scroll - 4.0f) {
                state_.protocol_auto_scroll = true;
            }
            if (state_.protocol_auto_scroll) {
                ImGui::SetScrollHereY(1.0f);
            }

            ImGui::PopStyleVar();
            ImGui::EndChild();
            ImGui::PopStyleVar();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    ImGui::End();
}
