#include <windows.h>
#include <DbgHelp.h>
#include <ShlObj.h>
#include <TlHelp32.h>
#include <chrono>
#include <filesystem>
#include <format>
#include <mutex>
#include <string>
#include <unordered_map>
#include <SafetyHook.hpp>
#include <spdlog/spdlog.h>

#include <utility/Module.hpp>
#include <utility/Scan.hpp>
#include <utility/Patch.hpp>
#include <utility/String.hpp>

#include "Framework.hpp"

#include "ExceptionHandler.hpp"

LONG WINAPI framework::global_exception_handler(struct _EXCEPTION_POINTERS* ei) {
    spdlog::flush_on(spdlog::level::err);

    spdlog::error("Exception occurred: {:x}", ei->ExceptionRecord->ExceptionCode);
    spdlog::error("RIP: {:x}", ei->ContextRecord->Rip);
    spdlog::error("RSP: {:x}", ei->ContextRecord->Rsp);
    spdlog::error("RCX: {:x}", ei->ContextRecord->Rcx);
    spdlog::error("RDX: {:x}", ei->ContextRecord->Rdx);
    spdlog::error("R8: {:x}", ei->ContextRecord->R8);
    spdlog::error("R9: {:x}", ei->ContextRecord->R9);
    spdlog::error("R10: {:x}", ei->ContextRecord->R10);
    spdlog::error("R11: {:x}", ei->ContextRecord->R11);
    spdlog::error("R12: {:x}", ei->ContextRecord->R12);
    spdlog::error("R13: {:x}", ei->ContextRecord->R13);
    spdlog::error("R14: {:x}", ei->ContextRecord->R14);
    spdlog::error("R15: {:x}", ei->ContextRecord->R15);
    spdlog::error("RAX: {:x}", ei->ContextRecord->Rax);
    spdlog::error("RBX: {:x}", ei->ContextRecord->Rbx);
    spdlog::error("RBP: {:x}", ei->ContextRecord->Rbp);
    spdlog::error("RSI: {:x}", ei->ContextRecord->Rsi);
    spdlog::error("RDI: {:x}", ei->ContextRecord->Rdi);
    spdlog::error("EFLAGS: {:x}", ei->ContextRecord->EFlags);
    spdlog::error("CS: {:x}", ei->ContextRecord->SegCs);
    spdlog::error("DS: {:x}", ei->ContextRecord->SegDs);
    spdlog::error("ES: {:x}", ei->ContextRecord->SegEs);
    spdlog::error("FS: {:x}", ei->ContextRecord->SegFs);
    spdlog::error("GS: {:x}", ei->ContextRecord->SegGs);
    spdlog::error("SS: {:x}", ei->ContextRecord->SegSs);

    const auto module_within = utility::get_module_within(ei->ContextRecord->Rip);

    if (module_within) {
        const auto module_path = utility::get_module_path(*module_within);

        if (module_path) {
            spdlog::error("Module: {:x} {}", (uintptr_t)*module_within, *module_path);
        } else {
            spdlog::error("Module: Unknown");
        }
    } else {
        spdlog::error("Module: Unknown");
    }

    auto dbghelp = LoadLibrary("dbghelp.dll");

    if (dbghelp) {
        const auto final_path = Framework::get_persistent_dir("crash.dmp").string();

        spdlog::error("Attempting to write dump to {}", final_path);

        auto f = CreateFile(final_path.c_str(), 
            GENERIC_WRITE, 
            FILE_SHARE_WRITE, 
            nullptr, 
            CREATE_ALWAYS, 
            FILE_ATTRIBUTE_NORMAL, 
            nullptr
        );

        if (!f || f == INVALID_HANDLE_VALUE) {  
            spdlog::error("Exception occurred, but could not create dump file");
            return EXCEPTION_CONTINUE_SEARCH;
        }

        MINIDUMP_EXCEPTION_INFORMATION ei_info{
            GetCurrentThreadId(),
            ei,
            FALSE
        };

        auto minidump_write_dump = (decltype(MiniDumpWriteDump)*)GetProcAddress(dbghelp, "MiniDumpWriteDump");

        minidump_write_dump(GetCurrentProcess(), 
            GetCurrentProcessId(),
            f,
            MINIDUMP_TYPE::MiniDumpNormal, 
            &ei_info, 
            nullptr, 
            nullptr
        );

        CloseHandle(f);
    } else {
        spdlog::error("Exception occurred, but could not load dbghelp.dll");
    }

    return EXCEPTION_EXECUTE_HANDLER;
}

// First-chance exception logger. A game's own fatal-error handler can terminate
// the process without our SetUnhandledExceptionFilter ever running, so we log
// every genuine error-class exception (and breakpoints — UE check()/fatal
// reaches here as EXCEPTION_BREAKPOINT via __debugbreak) with the faulting RIP
// (module+offset), registers, and a shallow stack scan, flushed to disk.
// CONTINUE_SEARCH always: this is a pure reporter, zero behavior change.
// Deduped by (RIP,code) so IsBadReadPtr-style probe storms can't flood the log;
// the LAST [FirstChance] entry before the log dies is the fatal one.
static LONG WINAPI first_chance_exception_logger(struct _EXCEPTION_POINTERS* ei) {
    const auto code = ei->ExceptionRecord->ExceptionCode;

    switch (code) {
    case 0x406D1388:  // MS-VC SetThreadName
    case 0x40010006:  // DBG_PRINTEXCEPTION_C
    case 0x4001000A:  // DBG_PRINTEXCEPTION_WIDE_C
        return EXCEPTION_CONTINUE_SEARCH;
    default:
        break;
    }

    // error-severity (0xCxxxxxxx), UE fatal RaiseException(1), or breakpoints.
    const bool interesting = (code & 0xF0000000u) == 0xC0000000u || code == 1 || code == EXCEPTION_BREAKPOINT;
    if (!interesting) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    static std::mutex log_mtx{};
    static std::unordered_map<uint64_t, uint64_t> seen_counts{};

    std::scoped_lock _{log_mtx};

    const auto rip = ei->ContextRecord->Rip;
    const auto key = (uint64_t)rip ^ ((uint64_t)code << 32);
    const auto count = ++seen_counts[key];

    if (count > 1) {
        if (count <= 4 || (count % 1000) == 0) {
            spdlog::error("[FirstChance] (repeat x{}) code {:x} thread {} RIP {:x}", count, code, GetCurrentThreadId(), rip);
            if (const auto logger = spdlog::default_logger()) { logger->flush(); }
        }
        return EXCEPTION_CONTINUE_SEARCH;
    }

    spdlog::error("[FirstChance] code {:x} thread {} RIP {:x} RSP {:x}", code, GetCurrentThreadId(), rip, ei->ContextRecord->Rsp);

    if (code == EXCEPTION_ACCESS_VIOLATION && ei->ExceptionRecord->NumberParameters >= 2) {
        const char* kind = ei->ExceptionRecord->ExceptionInformation[0] == 0 ? "read"
                         : ei->ExceptionRecord->ExceptionInformation[0] == 1 ? "write" : "execute(DEP)";
        spdlog::error("[FirstChance] AV {} of {:x}", kind, ei->ExceptionRecord->ExceptionInformation[1]);
    }

    if (const auto module_within = utility::get_module_within(rip)) {
        const auto module_path = utility::get_module_path(*module_within);
        spdlog::error("[FirstChance] RIP module: {} + {:x}", module_path.value_or("Unknown"), rip - (uintptr_t)*module_within);
    } else {
        spdlog::error("[FirstChance] RIP module: Unknown");
    }

    spdlog::error("[FirstChance] RAX {:x} RBX {:x} RCX {:x} RDX {:x} RSI {:x} RDI {:x} RBP {:x} R8 {:x} R9 {:x} R10 {:x} R11 {:x} R12 {:x} R13 {:x} R14 {:x} R15 {:x}",
        ei->ContextRecord->Rax, ei->ContextRecord->Rbx, ei->ContextRecord->Rcx, ei->ContextRecord->Rdx,
        ei->ContextRecord->Rsi, ei->ContextRecord->Rdi, ei->ContextRecord->Rbp,
        ei->ContextRecord->R8, ei->ContextRecord->R9, ei->ContextRecord->R10, ei->ContextRecord->R11,
        ei->ContextRecord->R12, ei->ContextRecord->R13, ei->ContextRecord->R14, ei->ContextRecord->R15);

    if (code != EXCEPTION_STACK_OVERFLOW) {
        const auto rsp = ei->ContextRecord->Rsp;
        int logged = 0;
        for (uintptr_t off = 0; off < 0x400 && logged < 12; off += sizeof(uintptr_t)) {
            const auto slot = rsp + off;
            if (IsBadReadPtr((void*)slot, sizeof(uintptr_t))) { break; }
            const auto val = *(uintptr_t*)slot;
            if (const auto mod = utility::get_module_within(val)) {
                const auto path = utility::get_module_path(*mod);
                spdlog::error("[FirstChance] Stack[rsp+{:x}]: {:x} ({} + {:x})", off, val, path.value_or("Unknown"), val - (uintptr_t)*mod);
                ++logged;
            }
        }
    } else {
        // A stack overflow's RIP is wherever the guard page happened to be hit —
        // often inside a lock or an allocator — so a single frame says nothing
        // about WHICH recursion ran away. The used stack ABOVE rsp is committed
        // and safe to read, and a runaway cycle stamps its return addresses into
        // it thousands of times. Count them and print the most frequent: that IS
        // the cycle. Fixed-size table, no heap, no unwinder — this runs on a
        // thread with one page of stack left.
        constexpr uintptr_t scan_bytes = 0x40000; // 256 KiB of already-used stack
        constexpr size_t max_tracked = 24;

        struct Frequent {
            uintptr_t addr{};
            uint32_t count{};
        };

        Frequent tracked[max_tracked]{};
        size_t tracked_count = 0;
        uint32_t matched = 0;
        uintptr_t scanned = 0;
        const auto rsp = ei->ContextRecord->Rsp;

        for (uintptr_t off = 0; off < scan_bytes; off += sizeof(uintptr_t)) {
            const auto slot = rsp + off;

            // Probe once per page instead of once per qword: each failed probe is
            // itself an exception, and 32k of them would take longer than the
            // process has left.
            if ((off & 0xfff) == 0 && IsBadReadPtr((void*)slot, 0x1000)) {
                break;
            }

            scanned = off;
            const auto val = *(uintptr_t*)slot;

            if (val < 0x10000 || !utility::get_module_within(val).has_value()) {
                continue;
            }

            ++matched;
            bool found = false;

            for (size_t i = 0; i < tracked_count; ++i) {
                if (tracked[i].addr == val) {
                    ++tracked[i].count;
                    found = true;
                    break;
                }
            }

            if (!found && tracked_count < max_tracked) {
                tracked[tracked_count].addr = val;
                tracked[tracked_count].count = 1;
                ++tracked_count;
            }
        }

        spdlog::error("[FirstChance] STACK OVERFLOW — scanned {:x} bytes of used stack, {} module return addresses, "
                      "{} distinct tracked. The repeated frames below are the recursion cycle:",
            scanned, matched, tracked_count);

        // Selection sort by count, descending — tiny table, and std::sort would
        // pull in more stack than this is worth here.
        for (size_t printed = 0; printed < tracked_count && printed < 10; ++printed) {
            size_t best = printed;

            for (size_t i = printed + 1; i < tracked_count; ++i) {
                if (tracked[i].count > tracked[best].count) {
                    best = i;
                }
            }

            const auto tmp = tracked[printed];
            tracked[printed] = tracked[best];
            tracked[best] = tmp;

            const auto addr = tracked[printed].addr;

            if (const auto mod = utility::get_module_within(addr)) {
                const auto path = utility::get_module_path(*mod);
                spdlog::error("[FirstChance]   x{:<6} {:x} ({} + {:x})",
                    tracked[printed].count, addr, path.value_or("Unknown"), addr - (uintptr_t)*mod);
            } else {
                spdlog::error("[FirstChance]   x{:<6} {:x}", tracked[printed].count, addr);
            }
        }
    }

    if (const auto logger = spdlog::default_logger()) { logger->flush(); }

    return EXCEPTION_CONTINUE_SEARCH;
}

// Some titles surface a fatal error as a bare MessageBox and then terminate.
// Hook it to capture the dialog text plus a real backtrace of whoever raised it
// — i.e. the fatal call site — before the process goes down.
static void log_backtrace(const char* tag) {
    void* frames[32]{};
    const auto n = RtlCaptureStackBackTrace(1, 32, frames, nullptr);
    for (USHORT i = 0; i < n; ++i) {
        const auto addr = (uintptr_t)frames[i];
        if (const auto mod = utility::get_module_within(addr)) {
            const auto path = utility::get_module_path(*mod);
            spdlog::error("{} Frame[{}]: {:x} ({} + {:x})", tag, i, addr, path.value_or("Unknown"), addr - (uintptr_t)*mod);
        } else {
            spdlog::error("{} Frame[{}]: {:x}", tag, i, addr);
        }
    }
}

static safetyhook::InlineHook g_messagebox_a_hook{};
static safetyhook::InlineHook g_messagebox_w_hook{};

static int WINAPI messagebox_a_hooked(HWND hwnd, LPCSTR text, LPCSTR caption, UINT type) {
    spdlog::error("[MessageBoxA] caption=\"{}\" text=\"{}\" thread {}",
        caption ? caption : "(null)", text ? text : "(null)", GetCurrentThreadId());
    log_backtrace("[MessageBoxA]");
    if (const auto logger = spdlog::default_logger()) { logger->flush(); }
    return g_messagebox_a_hook.call<int>(hwnd, text, caption, type);
}

static int WINAPI messagebox_w_hooked(HWND hwnd, LPCWSTR text, LPCWSTR caption, UINT type) {
    spdlog::error("[MessageBoxW] caption=\"{}\" text=\"{}\" thread {}",
        caption ? utility::narrow(caption) : "(null)", text ? utility::narrow(text) : "(null)", GetCurrentThreadId());
    log_backtrace("[MessageBoxW]");
    if (const auto logger = spdlog::default_logger()) { logger->flush(); }
    return g_messagebox_w_hook.call<int>(hwnd, text, caption, type);
}

void framework::setup_exception_handler() {
    SetUnhandledExceptionFilter(global_exception_handler);

    // See first_chance_exception_logger: catches fatal exceptions the game's own
    // handler would otherwise swallow before our unhandled filter runs.
    AddVectoredExceptionHandler(0 /* last — after any XRSystem patcher VEH */, first_chance_exception_logger);

    if (const auto user32 = GetModuleHandleW(L"user32.dll"); user32 != nullptr) {
        if (const auto fn = GetProcAddress(user32, "MessageBoxA"); fn != nullptr) {
            g_messagebox_a_hook = safetyhook::create_inline((void*)fn, (void*)&messagebox_a_hooked);
        }
        if (const auto fn = GetProcAddress(user32, "MessageBoxW"); fn != nullptr) {
            g_messagebox_w_hook = safetyhook::create_inline((void*)fn, (void*)&messagebox_w_hooked);
        }
    }
}

// --- Hang diagnostics --------------------------------------------------------
// A hang leaves nothing in the log: the last line is wherever each thread got
// to, which is not the same as where it is STUCK. This walks every other thread
// in the process and reports its current RIP plus the module-resident values
// still sitting on its stack - enough to name the function that is not
// returning, and (by comparing two dumps a while apart) to tell a blocked
// thread from a spinning one.
//
// Everything is copied out while the thread is suspended and only formatted
// after it is resumed: spdlog allocates, and allocating while another thread
// holds the CRT heap lock in suspension is its own deadlock. ReadProcessMemory
// against our own process reads the stack without IsBadReadPtr's first-chance
// AV storm.
void framework::dump_all_thread_stacks(const char* reason) {
    constexpr int max_dumps = 3;
    constexpr auto min_interval = std::chrono::seconds(20);
    constexpr size_t stack_bytes = 0x600;
    constexpr size_t max_frames = 14;

    static std::mutex mtx{};
    static int dumps = 0;
    static std::chrono::steady_clock::time_point last{};

    std::scoped_lock _{mtx};

    const auto now = std::chrono::steady_clock::now();

    if (dumps >= max_dumps || (dumps > 0 && now - last < min_interval)) {
        return;
    }
    last = now;
    ++dumps;

    const auto self = GetCurrentThreadId();
    const auto pid = GetCurrentProcessId();

    spdlog::error("[HangDump #{}] {} (dumping threads of pid {}, this is thread {})", dumps, reason, pid, self);

    const auto snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);

    if (snapshot == INVALID_HANDLE_VALUE) {
        spdlog::error("[HangDump #{}] Could not snapshot threads ({})", dumps, GetLastError());
        return;
    }

    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);

    for (auto ok = Thread32First(snapshot, &entry); ok; ok = Thread32Next(snapshot, &entry)) {
        if (entry.th32OwnerProcessID != pid || entry.th32ThreadID == self) {
            continue;
        }

        const auto thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION,
                                       FALSE, entry.th32ThreadID);

        if (thread == nullptr) {
            continue;
        }

        CONTEXT ctx{};
        ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;

        uint8_t stack[stack_bytes]{};
        SIZE_T stack_read = 0;
        bool have_ctx = false;
        // SuspendThread returns the PREVIOUS suspend count: anything above zero
        // means somebody else already had this thread frozen, which is the
        // difference between "stuck in a loop" and "not running at all".
        DWORD prev_suspend_count = 0;

        if (const auto count = SuspendThread(thread); count != (DWORD)-1) {
            prev_suspend_count = count;
            have_ctx = GetThreadContext(thread, &ctx) != FALSE;

            if (have_ctx) {
                ReadProcessMemory(GetCurrentProcess(), (void*)ctx.Rsp, stack, sizeof(stack), &stack_read);
            }

            ResumeThread(thread);
        }

        CloseHandle(thread);

        if (!have_ctx) {
            continue;
        }

        const auto describe = [](uintptr_t addr) {
            if (const auto module_within = utility::get_module_within(addr)) {
                const auto path = utility::get_module_path(*module_within);
                auto name = path.value_or("Unknown");

                if (const auto slash = name.find_last_of("\/"); slash != std::string::npos) {
                    name = name.substr(slash + 1);
                }

                return std::format("{} + {:x}", name, addr - (uintptr_t)*module_within);
            }

            return std::string{"<no module>"};
        };

        spdlog::error("[HangDump #{}] thread {} RIP {:x} ({}) RSP {:x} suspended_by_others={}", dumps,
                      entry.th32ThreadID, (uintptr_t)ctx.Rip, describe(ctx.Rip), (uintptr_t)ctx.Rsp,
                      prev_suspend_count);

        // Registers name the arguments of whatever call is stuck (a memcpy's
        // rcx/rdx/r8, say), and the region info says whether the addresses they
        // point at are even committed and writable.
        spdlog::error("[HangDump #{}]   RAX {:x} RBX {:x} RCX {:x} RDX {:x} RSI {:x} RDI {:x} RBP {:x} R8 {:x} R9 {:x}",
                      dumps, ctx.Rax, ctx.Rbx, ctx.Rcx, ctx.Rdx, ctx.Rsi, ctx.Rdi, ctx.Rbp, ctx.R8, ctx.R9);

        const auto describe_region = [&](const char* label, uintptr_t addr) {
            if (addr < 0x10000) {
                return;
            }

            MEMORY_BASIC_INFORMATION mbi{};

            if (VirtualQuery((void*)addr, &mbi, sizeof(mbi)) == 0) {
                spdlog::error("[HangDump #{}]   {} {:x}: unmapped", dumps, label, addr);
                return;
            }

            spdlog::error("[HangDump #{}]   {} {:x}: base {:x} size {:x} state {:x} protect {:x} type {:x}", dumps,
                          label, addr, (uintptr_t)mbi.BaseAddress, (uintptr_t)mbi.RegionSize, mbi.State, mbi.Protect,
                          mbi.Type);
        };

        describe_region("rcx", ctx.Rcx);
        describe_region("rdx", ctx.Rdx);
        describe_region("rsi", ctx.Rsi);
        describe_region("rdi", ctx.Rdi);

        // Frames only for threads that are actually somewhere interesting. A
        // thread parked in an ntdll wait is the normal state of most of a
        // game's thread pool, and dumping fourteen frames for each of eighty of
        // them buries the one thread that matters.
        const auto rip_module = utility::get_module_within(ctx.Rip);
        const auto rip_path = rip_module ? utility::get_module_path(*rip_module).value_or("") : std::string{};
        const bool parked = rip_path.find("ntdll.dll") != std::string::npos && prev_suspend_count == 0;

        if (parked) {
            continue;
        }

        size_t logged = 0;

        for (size_t off = 0; off + sizeof(uintptr_t) <= stack_read && logged < max_frames; off += sizeof(uintptr_t)) {
            uintptr_t value{};
            memcpy(&value, stack + off, sizeof(value));

            if (value < 0x10000 || utility::get_module_within(value) == std::nullopt) {
                continue;
            }

            spdlog::error("[HangDump #{}]   [rsp+{:x}] {:x} ({})", dumps, off, value, describe(value));
            ++logged;
        }
    }

    CloseHandle(snapshot);

    if (const auto logger = spdlog::default_logger()) {
        logger->flush();
    }
}
