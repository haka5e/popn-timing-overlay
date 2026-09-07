#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <MinHook.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace {

enum SpiceStatus : int { SPICE_SUCCESS = 0 };
enum SpiceLogLevel : int { SPICE_MISC = 0, SPICE_INFO = 1, SPICE_WARNING = 2, SPICE_FATAL = 3 };
using SpiceLog = int(__cdecl *)(SpiceLogLevel, const char *, const char *);
using SpiceToast = int(__cdecl *)(int, const char *);
using SpiceDestroy = void(__cdecl *)();
struct SpiceSdkV0 {
    uint32_t size;
    SpiceLog log;
    void *get_game_info;
    void *get_avs_info;
    void *get_button;
    void *set_button;
    void *get_analog;
    void *set_analog;
    void *get_light;
    void *set_light;
    void *set_touch;
    void *clear_touch;
    void *insert_card;
    void *set_keypad;
    SpiceToast add_toast;
};
using SpiceInit = int(__cdecl *)(uint32_t, SpiceDestroy, void *);

constexpr uintptr_t kJudgeRva = 0x1e1490;
constexpr uintptr_t kLaneWorkRva = 0x1078400;
constexpr uintptr_t kEarlyLimitRva = 0x105eed0;
constexpr uintptr_t kLateLimitRva = 0x105eee4;
constexpr size_t kLaneCount = 9;
constexpr size_t kMaxSamples = 128;
constexpr uint8_t kJudgeSignature[] = {
    0x48, 0x89, 0x5c, 0x24, 0x18, 0x56, 0x57, 0x41,
    0x54, 0x41, 0x55, 0x41, 0x57, 0x48, 0x83, 0xec, 0x20
};

struct Config {
    int range_ms = 50;
    int history = 64;
    int vertical_percent = 28;
    int width_pixels = 360;
    int height_pixels = 38;
    int reset_seconds = 12;
    int toggle_key = VK_F10;
    int overall_alpha = 255;
    int background_alpha = 42;
    int great_zone_alpha = 42;
    int cool_zone_alpha = 38;
    int zone_label_alpha = 125;
    int cool_early_ms = 22;
    int cool_late_ms = 18;
    uint32_t background_color = 0x000000;
    uint32_t great_zone_color = 0xff526b;
    uint32_t cool_zone_color = 0x39dce6;
    uint32_t zone_label_color = 0xd6e2e8;
    uint32_t axis_color = 0x82919b;
    uint32_t center_color = 0xdce6eb;
    uint32_t history_color = 0x1ee1e1;
    uint32_t latest_color = 0xfadc50;
    uint32_t label_color = 0x9baab4;
    uint32_t text_color = 0xd2e1e6;
    bool show_value = true;
    bool invert = false;
    bool judge_zones = true;
    bool zone_labels = true;
};

struct Sample {
    int delta_ms = 0;
    uint8_t judge = 0;
    uint8_t lane = 0;
    ULONGLONG tick = 0;
};

struct PixelBuffer {
    int width = 0;
    int height = 0;
    std::vector<uint32_t> pixels;
};

using JudgeFn = uint16_t(__fastcall *)(uint16_t, uint16_t, uint8_t, uint8_t, uint8_t);

HMODULE g_module = nullptr;
uintptr_t g_popn_base = 0;
SpiceSdkV0 g_spice{};
Config g_config{};
HANDLE g_stop_event = nullptr;
HANDLE g_worker = nullptr;
JudgeFn g_original_judge = nullptr;
void *g_judge_target = nullptr;
std::mutex g_samples_mutex;
std::array<Sample, kMaxSamples> g_samples{};
size_t g_sample_head = 0;
size_t g_sample_count = 0;
std::atomic<uint64_t> g_sample_generation{0};
std::atomic<bool> g_enabled{true};
std::atomic<bool> g_shutting_down{false};
bool g_toggle_was_down = false;

void log_message(SpiceLogLevel level, const char *message) {
    if (g_spice.log) {
        g_spice.log(level, "popn_timing", message);
    }
}

std::string module_directory() {
    char path[MAX_PATH]{};
    GetModuleFileNameA(g_module, path, MAX_PATH);
    char *slash = std::strrchr(path, '\\');
    if (slash) *slash = '\0';
    return path;
}

void load_config() {
    const std::string ini = module_directory() + "\\popn_timing_overlay.ini";
    auto read_int = [&](const char *name, int fallback) {
        return static_cast<int>(GetPrivateProfileIntA("Overlay", name, fallback, ini.c_str()));
    };
    auto read_color = [&](const char *name, uint32_t fallback) {
        char value[32]{};
        char fallback_text[16]{};
        std::snprintf(fallback_text, sizeof(fallback_text), "%06X", fallback & 0xffffff);
        GetPrivateProfileStringA("Overlay", name, fallback_text, value,
                                 static_cast<DWORD>(sizeof(value)), ini.c_str());
        const char *start = value;
        if (*start == '#') ++start;
        else if (start[0] == '0' && (start[1] == 'x' || start[1] == 'X')) start += 2;
        char *end = nullptr;
        const unsigned long parsed = std::strtoul(start, &end, 16);
        return end != start && *end == '\0' && parsed <= 0xffffff
            ? static_cast<uint32_t>(parsed) : fallback;
    };
    g_config.range_ms = std::clamp(read_int("RangeMs", 50), 20, 200);
    g_config.history = std::clamp(read_int("History", 64), 8, static_cast<int>(kMaxSamples));
    g_config.vertical_percent = std::clamp(read_int("VerticalPercent", 28), 5, 90);
    g_config.width_pixels = std::clamp(read_int("WidthPixels", 360), 180, 900);
    g_config.height_pixels = std::clamp(read_int("HeightPixels", 38), 24, 100);
    g_config.reset_seconds = std::clamp(read_int("ResetSeconds", 12), 2, 120);
    g_config.toggle_key = std::clamp(read_int("ToggleKey", VK_F10), 1, 255);
    g_config.overall_alpha = std::clamp(read_int("OverallAlpha", 255), 0, 255);
    g_config.background_alpha = std::clamp(read_int("BackgroundAlpha", 42), 0, 255);
    g_config.great_zone_alpha = std::clamp(read_int("GreatZoneAlpha", 42), 0, 255);
    g_config.cool_zone_alpha = std::clamp(read_int("CoolZoneAlpha", 38), 0, 255);
    g_config.zone_label_alpha = std::clamp(read_int("ZoneLabelAlpha", 125), 0, 255);
    g_config.cool_early_ms = std::clamp(read_int("CoolEarlyMs", 22), 1, 200);
    g_config.cool_late_ms = std::clamp(read_int("CoolLateMs", 18), 1, 200);
    g_config.background_color = read_color("BackgroundColor", 0x000000);
    g_config.great_zone_color = read_color("GreatZoneColor", 0xff526b);
    g_config.cool_zone_color = read_color("CoolZoneColor", 0x39dce6);
    g_config.zone_label_color = read_color("ZoneLabelColor", 0xd6e2e8);
    g_config.axis_color = read_color("AxisColor", 0x82919b);
    g_config.center_color = read_color("CenterColor", 0xdce6eb);
    g_config.history_color = read_color("HistoryColor", 0x1ee1e1);
    g_config.latest_color = read_color("LatestColor", 0xfadc50);
    g_config.label_color = read_color("LabelColor", 0x9baab4);
    g_config.text_color = read_color("TextColor", 0xd2e1e6);
    g_config.show_value = read_int("ShowValue", 1) != 0;
    g_config.invert = read_int("Invert", 0) != 0;
    g_config.judge_zones = read_int("JudgeZones", 1) != 0;
    g_config.zone_labels = read_int("ZoneLabels", 1) != 0;
}

void push_sample(int delta_ms, uint8_t judge, uint8_t lane) {
    if (g_config.invert) delta_ms = -delta_ms;
    std::lock_guard<std::mutex> lock(g_samples_mutex);
    g_samples[g_sample_head] = {delta_ms, judge, lane, GetTickCount64()};
    g_sample_head = (g_sample_head + 1) % kMaxSamples;
    g_sample_count = std::min(g_sample_count + 1, static_cast<size_t>(g_config.history));
    g_sample_generation.fetch_add(1, std::memory_order_relaxed);
}

uint16_t __fastcall hook_judge(uint16_t lane, uint16_t value, uint8_t active,
                               uint8_t allow_miss, uint8_t extra) {
    int delta = 0;
    int early_limit = 0;
    int late_limit = 0;
    bool had_note = false;
    const size_t lane_index = static_cast<size_t>(lane);
    if (g_popn_base && lane_index < kLaneCount) {
        const uintptr_t work = g_popn_base + kLaneWorkRva + lane_index * 0x10;
        had_note = *reinterpret_cast<const uintptr_t *>(work) != 0;
        delta = *reinterpret_cast<const int *>(work + 8);
        early_limit = *reinterpret_cast<const int *>(g_popn_base + kEarlyLimitRva);
        late_limit = *reinterpret_cast<const int *>(g_popn_base + kLateLimitRva);
    }

    const uint16_t result = g_original_judge(lane, value, active, allow_miss, extra);
    const uint8_t judge = static_cast<uint8_t>(result & 0xff);
    const bool inside_window = early_limit < late_limit &&
        delta >= early_limit && delta <= late_limit;
    if (!g_shutting_down.load(std::memory_order_relaxed) && had_note && inside_window &&
        judge != 7 && judge != 10 && judge != 11) {
        push_sample(delta, judge, static_cast<uint8_t>(lane_index));
    }
    return result;
}

uint32_t color(uint8_t a, uint8_t r, uint8_t g, uint8_t b) {
    a = static_cast<uint8_t>((static_cast<unsigned int>(a) *
        static_cast<unsigned int>(g_config.overall_alpha) + 127) / 255);
    return (static_cast<uint32_t>(a) << 24) | (static_cast<uint32_t>(r) << 16) |
        (static_cast<uint32_t>(g) << 8) | b;
}

uint32_t color_from_rgb(uint8_t alpha, uint32_t rgb) {
    return color(alpha, static_cast<uint8_t>((rgb >> 16) & 0xff),
        static_cast<uint8_t>((rgb >> 8) & 0xff), static_cast<uint8_t>(rgb & 0xff));
}

void blend_pixel(uint32_t &destination, uint32_t source) {
    const uint32_t sa = source >> 24;
    if (!sa) return;
    const uint32_t inv = 255 - sa;
    const uint32_t da = destination >> 24;
    const uint32_t dr = (destination >> 16) & 0xff;
    const uint32_t dg = (destination >> 8) & 0xff;
    const uint32_t db = destination & 0xff;
    const uint32_t sr = ((source >> 16) & 0xff) * sa / 255;
    const uint32_t sg = ((source >> 8) & 0xff) * sa / 255;
    const uint32_t sb = (source & 0xff) * sa / 255;
    const uint32_t oa = sa + da * inv / 255;
    const uint32_t or_ = sr + dr * inv / 255;
    const uint32_t og = sg + dg * inv / 255;
    const uint32_t ob = sb + db * inv / 255;
    destination = (oa << 24) | (or_ << 16) | (og << 8) | ob;
}

void add_rect(PixelBuffer &buffer, float x0, float y0, float x1, float y1, uint32_t c) {
    const int left = std::clamp(static_cast<int>(std::floor(x0)), 0, buffer.width);
    const int top = std::clamp(static_cast<int>(std::floor(y0)), 0, buffer.height);
    const int right = std::clamp(static_cast<int>(std::ceil(x1)), 0, buffer.width);
    const int bottom = std::clamp(static_cast<int>(std::ceil(y1)), 0, buffer.height);
    for (int y = top; y < bottom; ++y) {
        uint32_t *row = buffer.pixels.data() + static_cast<size_t>(y) * buffer.width;
        for (int x = left; x < right; ++x) blend_pixel(row[x], c);
    }
}

const uint8_t *glyph(char ch) {
    static const uint8_t A[7]={14,17,17,31,17,17,17}, E[7]={31,16,16,30,16,16,31};
    static const uint8_t L[7]={16,16,16,16,16,16,31}, R[7]={30,17,17,30,20,18,17};
    static const uint8_t Y[7]={17,17,10,4,4,4,4}, T[7]={31,4,4,4,4,4,4};
    static const uint8_t M[7]={17,27,21,21,17,17,17}, S[7]={15,16,16,14,1,1,30};
    static const uint8_t P[7]={30,17,17,30,16,16,16}, N[7]={17,25,21,19,17,17,17};
    static const uint8_t C[7]={14,17,16,16,16,17,14}, G[7]={14,17,16,23,17,17,14};
    static const uint8_t O[7]={14,17,17,17,17,17,14};
    static const uint8_t D0[7]={14,17,19,21,25,17,14}, D1[7]={4,12,4,4,4,4,14};
    static const uint8_t D2[7]={14,17,1,2,4,8,31}, D3[7]={30,1,1,14,1,1,30};
    static const uint8_t D4[7]={2,6,10,18,31,2,2}, D5[7]={31,16,16,30,1,1,30};
    static const uint8_t D6[7]={14,16,16,30,17,17,14}, D7[7]={31,1,2,4,8,8,8};
    static const uint8_t D8[7]={14,17,17,14,17,17,14}, D9[7]={14,17,17,15,1,1,14};
    static const uint8_t PLUS[7]={0,4,4,31,4,4,0}, MINUS[7]={0,0,0,31,0,0,0};
    static const uint8_t SPACE[7]={0,0,0,0,0,0,0};
    switch (ch) {
        case 'A': return A; case 'E': return E; case 'L': return L; case 'R': return R;
        case 'Y': return Y; case 'T': return T; case 'M': return M; case 'S': return S;
        case 'P': return P; case 'N': return N; case 'C': return C; case 'G': return G;
        case 'O': return O; case '0': return D0; case '1': return D1;
        case '2': return D2; case '3': return D3; case '4': return D4; case '5': return D5;
        case '6': return D6; case '7': return D7; case '8': return D8; case '9': return D9;
        case '+': return PLUS; case '-': return MINUS; default: return SPACE;
    }
}

float text_width(const char *s, float scale) {
    return static_cast<float>(std::strlen(s)) * 6.0f * scale - scale;
}

void add_text(PixelBuffer &buffer, float x, float y, const char *s, float scale, uint32_t c) {
    for (; *s; ++s, x += 6.0f * scale) {
        const uint8_t *rows = glyph(*s);
        for (int row = 0; row < 7; ++row) {
            for (int col = 0; col < 5; ++col) {
                if (rows[row] & (1u << (4 - col))) {
                    const float px = x + col * scale;
                    const float py = y + row * scale;
                    add_rect(buffer, px, py, px + scale, py + scale, c);
                }
            }
        }
    }
}

void poll_toggle_key() {
    const bool key_down = (GetAsyncKeyState(g_config.toggle_key) & 0x8000) != 0;
    if (key_down && !g_toggle_was_down) {
        g_enabled.store(!g_enabled.load());
        g_sample_generation.fetch_add(1, std::memory_order_relaxed);
        log_message(SPICE_INFO, g_enabled.load() ? "overlay enabled" : "overlay disabled");
    }
    g_toggle_was_down = key_down;
}

size_t snapshot_samples(std::array<Sample, kMaxSamples> &snapshot) {
    size_t count = 0;
    {
        std::lock_guard<std::mutex> lock(g_samples_mutex);
        if (g_sample_count) {
            const size_t newest = (g_sample_head + kMaxSamples - 1) % kMaxSamples;
            if (GetTickCount64() - g_samples[newest].tick >
                static_cast<ULONGLONG>(g_config.reset_seconds) * 1000) {
                g_sample_count = 0;
                g_sample_generation.fetch_add(1, std::memory_order_relaxed);
            }
        }
        count = g_sample_count;
        const size_t start = (g_sample_head + kMaxSamples - count) % kMaxSamples;
        for (size_t i = 0; i < count; ++i) snapshot[i] = g_samples[(start + i) % kMaxSamples];
    }
    return count;
}

bool build_overlay_frame(PixelBuffer &buffer, int viewport_width, int viewport_height) {
    std::array<Sample, kMaxSamples> snapshot{};
    const size_t count = snapshot_samples(snapshot);
    if (!count || viewport_width < 320 || viewport_height < 240) return false;
    buffer.width = viewport_width;
    buffer.height = viewport_height;
    buffer.pixels.assign(static_cast<size_t>(viewport_width) * viewport_height, 0);

    const float width = std::min(static_cast<float>(g_config.width_pixels), viewport_width * 0.72f);
    const float center = viewport_width * 0.5f;
    const float axis_y = viewport_height * (g_config.vertical_percent / 100.0f);
    const float left = center - width * 0.5f;
    const float right = center + width * 0.5f;
    const float font_scale = std::clamp(viewport_height / 720.0f, 1.0f, 2.0f);
    const float half_height = g_config.height_pixels * 0.5f;
    const int early_cool_ms = std::min(g_config.cool_early_ms, g_config.range_ms);
    const int late_cool_ms = std::min(g_config.cool_late_ms, g_config.range_ms);
    const int left_cool_ms = g_config.invert ? late_cool_ms : early_cool_ms;
    const int right_cool_ms = g_config.invert ? early_cool_ms : late_cool_ms;
    const float cool_left = center - left_cool_ms /
        static_cast<float>(g_config.range_ms) * width * 0.5f;
    const float cool_right = center + right_cool_ms /
        static_cast<float>(g_config.range_ms) * width * 0.5f;
    const float zone_top = axis_y - half_height;
    const float zone_bottom = axis_y + half_height;

    add_rect(buffer, left - 5, zone_top - 1, right + 5, zone_bottom + 1,
             color_from_rgb(g_config.background_alpha, g_config.background_color));
    if (g_config.judge_zones) {
        const uint32_t great_color = color_from_rgb(
            g_config.great_zone_alpha, g_config.great_zone_color);
        add_rect(buffer, left, zone_top, cool_left, zone_bottom, great_color);
        add_rect(buffer, cool_right, zone_top, right, zone_bottom, great_color);
        add_rect(buffer, cool_left, zone_top, cool_right, zone_bottom,
                 color_from_rgb(g_config.cool_zone_alpha, g_config.cool_zone_color));
    }
    for (float x = left; x <= right; x += 8.0f) {
        add_rect(buffer, x, axis_y, x + 3.0f, axis_y + 1.0f,
                 color_from_rgb(115, g_config.axis_color));
    }
    add_rect(buffer, center - 1.0f, zone_top + 1.0f, center + 1.0f, zone_bottom - 1.0f,
             color_from_rgb(205, g_config.center_color));

    if (g_config.judge_zones) {
        add_rect(buffer, cool_left - 0.5f, zone_top + 2.0f,
                 cool_left + 0.5f, zone_bottom - 2.0f,
                 color_from_rgb(72, g_config.center_color));
        add_rect(buffer, cool_right - 0.5f, zone_top + 2.0f,
                 cool_right + 0.5f, zone_bottom - 2.0f,
                 color_from_rgb(72, g_config.center_color));
    }

    if (g_config.judge_zones && g_config.zone_labels) {
        const float zone_scale = std::max(1.0f, font_scale * 0.78f);
        const float label_y = axis_y - 3.5f * zone_scale;
        const uint32_t label_color = color_from_rgb(
            g_config.zone_label_alpha, g_config.zone_label_color);
        add_text(buffer, (left + cool_left - text_width("GREAT", zone_scale)) * 0.5f,
                 label_y, "GREAT", zone_scale, label_color);
        add_text(buffer, (cool_left + cool_right - text_width("COOL", zone_scale)) * 0.5f,
                 label_y, "COOL", zone_scale, label_color);
        add_text(buffer, (cool_right + right - text_width("GREAT", zone_scale)) * 0.5f,
                 label_y, "GREAT", zone_scale, label_color);
    }

    for (size_t i = 0; i < count; ++i) {
        const Sample &sample = snapshot[i];
        const float normalized = std::clamp(sample.delta_ms /
            static_cast<float>(g_config.range_ms), -1.0f, 1.0f);
        const float x = center + normalized * width * 0.5f;
        const float age = count > 1 ? i / static_cast<float>(count - 1) : 1.0f;
        const uint8_t alpha = static_cast<uint8_t>(70 + age * 175);
        uint32_t bar = color_from_rgb(alpha, g_config.history_color);
        if (i + 1 == count) bar = color_from_rgb(245, g_config.latest_color);
        add_rect(buffer, x - 2.0f, zone_top + 3.0f, x + 2.0f, zone_bottom - 3.0f, bar);
    }

    add_rect(buffer, center - 1.0f, axis_y + 20.0f, center + 1.0f, axis_y + 22.0f,
             color_from_rgb(220, g_config.center_color));
    add_rect(buffer, center - 2.0f, axis_y + 22.0f, center + 2.0f, axis_y + 24.0f,
             color_from_rgb(220, g_config.center_color));
    add_rect(buffer, center - 4.0f, axis_y + 24.0f, center + 4.0f, axis_y + 27.0f,
             color_from_rgb(220, g_config.center_color));
    add_text(buffer, left - text_width("EARLY", font_scale) - 9.0f, axis_y - 4.0f * font_scale,
             "EARLY", font_scale, color_from_rgb(175, g_config.label_color));
    add_text(buffer, right + 9.0f, axis_y - 4.0f * font_scale,
             "LATE", font_scale, color_from_rgb(175, g_config.label_color));

    if (g_config.show_value) {
        char value[24]{};
        std::snprintf(value, sizeof(value), "%+d MS", snapshot[count - 1].delta_ms);
        const float value_scale = font_scale * 0.9f;
        add_text(buffer, center - text_width(value, value_scale) * 0.5f, axis_y + 33.0f,
                 value, value_scale, color_from_rgb(205, g_config.text_color));
    }
    return true;
}

LRESULT CALLBACK overlay_window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_NCHITTEST) return HTTRANSPARENT;
    if (message == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    if (message == WM_ERASEBKGND) return 1;
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

struct WindowCandidate {
    HWND excluded = nullptr;
    HWND best = nullptr;
    LONG area = 0;
};

BOOL CALLBACK find_game_window_callback(HWND hwnd, LPARAM value) {
    auto &candidate = *reinterpret_cast<WindowCandidate *>(value);
    if (hwnd == candidate.excluded || !IsWindowVisible(hwnd) || IsIconic(hwnd)) return TRUE;
    DWORD process_id = 0;
    GetWindowThreadProcessId(hwnd, &process_id);
    if (process_id != GetCurrentProcessId()) return TRUE;
    RECT client{};
    if (!GetClientRect(hwnd, &client)) return TRUE;
    const LONG area = (client.right - client.left) * (client.bottom - client.top);
    if (area > candidate.area) {
        candidate.area = area;
        candidate.best = hwnd;
    }
    return TRUE;
}

HWND find_game_window(HWND overlay) {
    WindowCandidate candidate{overlay};
    EnumWindows(find_game_window_callback, reinterpret_cast<LPARAM>(&candidate));
    return candidate.best;
}

bool game_window_rect(HWND game, RECT &screen_rect) {
    if (!game || !IsWindow(game) || !IsWindowVisible(game) || IsIconic(game)) return false;
    RECT client{};
    if (!GetClientRect(game, &client)) return false;
    POINT origin{client.left, client.top};
    if (!ClientToScreen(game, &origin)) return false;
    screen_rect = {origin.x, origin.y, origin.x + client.right - client.left,
                   origin.y + client.bottom - client.top};
    return screen_rect.right - screen_rect.left >= 320 && screen_rect.bottom - screen_rect.top >= 240;
}

bool game_has_focus(HWND game) {
    const HWND foreground = GetForegroundWindow();
    if (!foreground || !game) return false;
    return GetAncestor(foreground, GA_ROOT) == GetAncestor(game, GA_ROOT);
}

bool present_overlay(HWND overlay, const RECT &screen_rect, const PixelBuffer &buffer) {
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = buffer.width;
    info.bmiHeader.biHeight = -buffer.height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void *bits = nullptr;
    HDC screen_dc = GetDC(nullptr);
    HDC memory_dc = CreateCompatibleDC(screen_dc);
    HBITMAP bitmap = CreateDIBSection(screen_dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!screen_dc || !memory_dc || !bitmap || !bits) {
        if (bitmap) DeleteObject(bitmap);
        if (memory_dc) DeleteDC(memory_dc);
        if (screen_dc) ReleaseDC(nullptr, screen_dc);
        return false;
    }
    std::memcpy(bits, buffer.pixels.data(), buffer.pixels.size() * sizeof(uint32_t));
    HGDIOBJ old_bitmap = SelectObject(memory_dc, bitmap);
    POINT position{screen_rect.left, screen_rect.top};
    SIZE size{buffer.width, buffer.height};
    POINT source{};
    BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    const BOOL updated = UpdateLayeredWindow(overlay, screen_dc, &position, &size,
        memory_dc, &source, 0, &blend, ULW_ALPHA);
    SelectObject(memory_dc, old_bitmap);
    DeleteObject(bitmap);
    DeleteDC(memory_dc);
    ReleaseDC(nullptr, screen_dc);
    return updated != FALSE;
}

bool install_judge_hook() {
    HMODULE popn = nullptr;
    for (int i = 0; i < 1200 && WaitForSingleObject(g_stop_event, 100) == WAIT_TIMEOUT; ++i) {
        popn = GetModuleHandleW(L"popn.dll");
        if (popn) break;
    }
    if (!popn) {
        log_message(SPICE_WARNING, "popn.dll was not loaded; timing hook disabled");
        return false;
    }
    g_popn_base = reinterpret_cast<uintptr_t>(popn);
    const void *target = reinterpret_cast<void *>(g_popn_base + kJudgeRva);
    if (std::memcmp(target, kJudgeSignature, sizeof(kJudgeSignature)) != 0) {
        log_message(SPICE_WARNING, "unsupported popn.dll build; signature mismatch");
        if (g_spice.add_toast) g_spice.add_toast(2, "Timing overlay: unsupported popn.dll build");
        return false;
    }
    g_judge_target = const_cast<void *>(target);
    if (MH_CreateHook(g_judge_target, hook_judge,
                      reinterpret_cast<void **>(&g_original_judge)) != MH_OK ||
        MH_EnableHook(g_judge_target) != MH_OK) {
        log_message(SPICE_WARNING, "failed to install judgment hook");
        return false;
    }
    log_message(SPICE_INFO, "judgment timing hook ready");
    return true;
}

DWORD WINAPI worker_main(void *) {
    load_config();
    const MH_STATUS init_status = MH_Initialize();
    if (init_status != MH_OK && init_status != MH_ERROR_ALREADY_INITIALIZED) {
        log_message(SPICE_WARNING, "MinHook initialization failed");
        return 0;
    }
    const bool judge_ok = install_judge_hook();

    constexpr wchar_t kOverlayClass[] = L"PopnTimingOverlayWindow";
    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = overlay_window_proc;
    window_class.hInstance = g_module;
    window_class.lpszClassName = kOverlayClass;
    const ATOM registered = RegisterClassExW(&window_class);
    const bool class_ok = registered != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    HWND overlay = nullptr;
    if (class_ok) {
        overlay = CreateWindowExW(
            WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST,
            kOverlayClass, L"pop'n timing", WS_POPUP, 0, 0, 1, 1,
            nullptr, nullptr, g_module, nullptr);
    }
    const bool window_ok = overlay != nullptr;
    if (window_ok) log_message(SPICE_INFO, "transparent overlay window ready");
    else log_message(SPICE_WARNING, "failed to create transparent overlay window");

    if (judge_ok && window_ok) {
        log_message(SPICE_INFO, "real-time timing overlay ready (F10 toggles it)");
        if (g_spice.add_toast) g_spice.add_toast(1, "Real-time timing overlay ready (F10)");
    } else {
        log_message(SPICE_WARNING, "timing overlay started with one or more hooks unavailable");
    }

    HWND game_window = nullptr;
    RECT last_rect{-1, -1, -1, -1};
    uint64_t last_generation = UINT64_MAX;
    unsigned int scan_counter = 0;
    bool overlay_visible = false;
    bool drawing_logged = false;
    PixelBuffer frame;
    while (WaitForSingleObject(g_stop_event, 16) == WAIT_TIMEOUT) {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        poll_toggle_key();

        if (!game_window || !IsWindow(game_window) || (++scan_counter % 60) == 0) {
            game_window = find_game_window(overlay);
        }
        RECT current_rect{};
        const bool valid_window = game_window_rect(game_window, current_rect);
        std::array<Sample, kMaxSamples> current_samples{};
        const bool has_samples = snapshot_samples(current_samples) != 0;
        const bool should_show = window_ok && valid_window && has_samples &&
            g_enabled.load(std::memory_order_relaxed) && game_has_focus(game_window);
        if (!should_show) {
            if (overlay_visible) {
                ShowWindow(overlay, SW_HIDE);
                overlay_visible = false;
            }
            continue;
        }

        const uint64_t generation = g_sample_generation.load(std::memory_order_relaxed);
        const bool rect_changed = std::memcmp(&current_rect, &last_rect, sizeof(RECT)) != 0;
        if (generation != last_generation || rect_changed || !overlay_visible) {
            const int width = current_rect.right - current_rect.left;
            const int height = current_rect.bottom - current_rect.top;
            if (build_overlay_frame(frame, width, height) &&
                present_overlay(overlay, current_rect, frame)) {
                SetWindowPos(overlay, HWND_TOPMOST, 0, 0, 0, 0,
                    SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
                overlay_visible = true;
                if (!drawing_logged) {
                    log_message(SPICE_INFO, "overlay drawing active");
                    drawing_logged = true;
                }
                last_generation = generation;
                last_rect = current_rect;
            }
        }
    }
    if (overlay) DestroyWindow(overlay);
    if (registered) UnregisterClassW(kOverlayClass, g_module);
    return 0;
}

void __cdecl destroy_plugin() {
    g_shutting_down.store(true);
    if (g_stop_event) SetEvent(g_stop_event);
    if (g_worker) {
        WaitForSingleObject(g_worker, 5000);
        CloseHandle(g_worker);
        g_worker = nullptr;
    }
    if (g_judge_target) MH_DisableHook(g_judge_target);
    MH_Uninitialize();
    if (g_stop_event) {
        CloseHandle(g_stop_event);
        g_stop_event = nullptr;
    }
    log_message(SPICE_INFO, "plugin unloaded");
}

} // namespace

extern "C" __declspec(dllexport) int __cdecl spice_sdk_entry_point(SpiceInit init) {
    g_spice = {};
    g_spice.size = sizeof(g_spice);
    if (!init || init(0, destroy_plugin, &g_spice) != SPICE_SUCCESS) return 0;
    log_message(SPICE_INFO, "plugin loaded");
    g_stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!g_stop_event) return 0;
    g_worker = CreateThread(nullptr, 0, worker_main, nullptr, 0, nullptr);
    if (!g_worker) {
        CloseHandle(g_stop_event);
        g_stop_event = nullptr;
        return 0;
    }
    return 1;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = instance;
        DisableThreadLibraryCalls(instance);
    }
    return TRUE;
}
