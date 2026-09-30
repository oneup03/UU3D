#pragma once

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <optional>

#include <windows.h>
#include <dxgi.h>
#include <wrl/client.h>

#include <spdlog/spdlog.h>

// Holds the present thread on the display's vblank so a forced sync interval
// is honoured even when DXGI itself does not block.
//
// DXGI only throttles an interval-N present while DWM is compositing the
// window. When the game window is covered by another process's fullscreen
// output (a Katanga consumer: VRto3D's DX9 window, NV3D-Glass's FSE popup, the
// WibbleWobble client) the present returns immediately and the game's only
// pacer becomes the t.MaxFPS timer - a QPC clock that drifts against the
// panel's pixel clock and beats with it (one dropped or doubled world update
// every few minutes). The guard restores the display as the clock: after the
// original Present returns, wait on the containing output's vblank until
// `interval` refreshes have passed since the previous paced release.
//
// Rule: wait only if less than (interval - 0.5) periods have elapsed since the
// last release. A present DXGI already blocked on returns at ~interval periods
// and never waits; a late (GPU-bound) frame never waits (mailbox behaviour, no
// vsync halving); only an unthrottled early return waits, and it is always
// released on a vblank edge. The wall clock merely counts vblanks with half a
// period of slack, so the pace is the panel's clock, not the timer's.
//
// One-shot, like the hooks' other present overrides: request() before the
// present, after_present() once it returned. Owned by the present thread; the
// stats are atomics so the flat3d update (another thread) can read them.
class DXGIVBlankGuard {
public:
    struct Stats {
        uint32_t presents{0}; // paced presents since the last consume
        uint32_t waits{0};    // of those, how many had to wait on vblank
        bool unavailable{false};
    };

    void request(uint32_t interval, double period_ms) {
        m_next = Request{interval, period_ms};
    }

    bool unavailable() const {
        return m_unavailable.load(std::memory_order_relaxed);
    }

    // Call right after the original Present returned (or was skipped:
    // presented == false consumes the request without waiting).
    void after_present(IDXGISwapChain* swap_chain, bool presented) {
        if (!m_next.has_value()) {
            return;
        }

        const auto req = *m_next;
        m_next.reset();

        if (!presented || swap_chain == nullptr || req.interval == 0) {
            return;
        }

        m_presents.fetch_add(1, std::memory_order_relaxed);

        using clock = std::chrono::steady_clock;
        const auto now = clock::now();

        // Prefer the period measured from back-to-back vblank waits over the
        // integer DEVMODE refresh (a 59.94 Hz mode reports 59 or 60) - unless
        // they disagree outright, which means the display mode changed under
        // the same monitor and the measurement is stale.
        if (m_measured_period_ms > 0.0 && req.period_ms > 0.0 &&
            std::abs(m_measured_period_ms - req.period_ms) > 0.25 * req.period_ms) {
            m_measured_period_ms = 0.0;
        }

        const double period_ms = m_measured_period_ms > 0.0 ? m_measured_period_ms : req.period_ms;

        if (period_ms <= 0.0) {
            m_last_release = now;
            return;
        }

        const bool have_last = m_last_release.time_since_epoch().count() != 0;
        const double since_last_ms = have_last ? ms_between(m_last_release, now) : 1e9;
        const double want_ms = ((double)req.interval - 0.5) * period_ms;

        if (since_last_ms >= want_ms) {
            // DXGI blocked for us, or the frame is already late: release now.
            m_last_release = now;
            return;
        }

        auto* output = resolve_output(swap_chain);

        if (output == nullptr) {
            m_last_release = now;
            return;
        }

        // Bounded: never more than `interval` waits per present, so a bad
        // period or a driver whose wait returns immediately cannot hang a
        // frame or halve the rate.
        uint32_t waited = 0;
        auto prev = clock::now();

        while (waited < req.interval) {
            const auto hr = output->WaitForVBlank();

            if (FAILED(hr)) {
                mark_unavailable("WaitForVBlank failed", hr);
                break;
            }

            ++waited;
            const auto t = clock::now();

            // Two consecutive waits straddle exactly one refresh: measure it.
            if (waited >= 2) {
                const double d = ms_between(prev, t);
                if (d > 1.0 && d < 100.0) {
                    m_measured_period_ms = m_measured_period_ms > 0.0 ? (m_measured_period_ms * 0.9 + d * 0.1) : d;
                }
            }
            prev = t;

            if (ms_between(m_last_release, t) >= want_ms) {
                break;
            }
        }

        if (waited > 0) {
            m_waits.fetch_add(1, std::memory_order_relaxed);
        }

        m_last_release = clock::now();
    }

    // Returns the counters since the previous call and resets them.
    Stats consume_stats() {
        Stats s{};
        s.presents = m_presents.exchange(0, std::memory_order_relaxed);
        s.waits = m_waits.exchange(0, std::memory_order_relaxed);
        s.unavailable = m_unavailable.load(std::memory_order_relaxed);
        return s;
    }

    double measured_period_ms() const {
        return m_measured_period_ms;
    }

    // Drop the cached output and phase (mode switch, swapchain recreation).
    void reset() {
        m_next.reset();
        m_output.Reset();
        m_monitor = nullptr;
        m_last_release = {};
        m_measured_period_ms = 0.0;
        m_unavailable.store(false, std::memory_order_relaxed);
    }

private:
    struct Request {
        uint32_t interval{1};
        double period_ms{0.0};
    };

    static double ms_between(std::chrono::steady_clock::time_point a, std::chrono::steady_clock::time_point b) {
        return std::chrono::duration<double, std::milli>(b - a).count();
    }

    void mark_unavailable(const char* what, HRESULT hr) {
        if (!m_unavailable.exchange(true, std::memory_order_relaxed)) {
            spdlog::warn("[Flat3D] VBlank guard: {} (0x{:x}) - falling back to the t.MaxFPS cap", what, (uint32_t)hr);
        }
        m_output.Reset();
        m_monitor = nullptr;
    }

    IDXGIOutput* resolve_output(IDXGISwapChain* swap_chain) {
        DXGI_SWAP_CHAIN_DESC desc{};
        swap_chain->GetDesc(&desc);

        const HMONITOR mon = desc.OutputWindow != nullptr
                                 ? MonitorFromWindow(desc.OutputWindow, MONITOR_DEFAULTTONEAREST)
                                 : nullptr;

        if (m_output != nullptr && mon == m_monitor) {
            return m_output.Get();
        }

        Microsoft::WRL::ComPtr<IDXGIOutput> out{};
        const auto hr = swap_chain->GetContainingOutput(&out);

        if (FAILED(hr) || out == nullptr) {
            mark_unavailable("GetContainingOutput failed", hr);
            return nullptr;
        }

        m_output = out;
        m_monitor = mon;
        m_measured_period_ms = 0.0;

        if (m_unavailable.exchange(false, std::memory_order_relaxed)) {
            spdlog::info("[Flat3D] VBlank guard: output available again");
        }

        DXGI_OUTPUT_DESC od{};
        if (SUCCEEDED(m_output->GetDesc(&od))) {
            char name[64]{};
            for (size_t i = 0; i < 63 && od.DeviceName[i] != 0; ++i) {
                name[i] = (char)od.DeviceName[i];
            }
            spdlog::info("[Flat3D] VBlank guard: pacing on {}", name);
        }

        return m_output.Get();
    }

    std::optional<Request> m_next{};
    Microsoft::WRL::ComPtr<IDXGIOutput> m_output{};
    HMONITOR m_monitor{nullptr};
    std::chrono::steady_clock::time_point m_last_release{};
    double m_measured_period_ms{0.0};
    std::atomic<uint32_t> m_presents{0};
    std::atomic<uint32_t> m_waits{0};
    std::atomic<bool> m_unavailable{false};
};
