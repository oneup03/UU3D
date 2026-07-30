#include <windows.h>
#include <DbgHelp.h>
#include <ShlObj.h>
#include <filesystem>
#include <mutex>
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
