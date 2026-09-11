#include <windows.h>
#include <DbgHelp.h>
#include <ShlObj.h>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <filesystem>
#include <string>
#include <spdlog/spdlog.h>

#include <utility/Module.hpp>
#include <utility/Scan.hpp>
#include <utility/Patch.hpp>

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

void framework::setup_exception_handler() {
    SetUnhandledExceptionFilter(global_exception_handler);
}

// ---------------------------------------------------------------------------
// First-chance exception logger (diagnostics)
//
// UE shipping builds wrap their threads in __try/__except, so a hardware fault
// on the game thread reaches the engine's crash reporter and never our
// SetUnhandledExceptionFilter. A vectored handler runs before any frame-based
// SEH dispatch, so this is the only place we can record where a fault happened.
// ---------------------------------------------------------------------------
namespace {
std::atomic<bool> g_first_chance_armed{false};
std::atomic<uint32_t> g_first_chance_logged{0};
constexpr uint32_t FIRST_CHANCE_TOTAL_CAP = 300;
constexpr uint32_t FIRST_CHANCE_PER_RIP_CAP = 3;

struct RipBucket {
    std::atomic<uintptr_t> rip{0};
    std::atomic<uint32_t> count{0};
};

RipBucket g_rip_buckets[128]{};

bool is_interesting_exception(DWORD code) {
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:
    case EXCEPTION_IN_PAGE_ERROR:
    case EXCEPTION_ILLEGAL_INSTRUCTION:
    case EXCEPTION_PRIV_INSTRUCTION:
    case EXCEPTION_STACK_OVERFLOW:
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
    case EXCEPTION_DATATYPE_MISALIGNMENT:
    case 0xC0000409: // STATUS_STACK_BUFFER_OVERRUN (__fastfail / GS cookie)
        return true;
    default:
        return false;
    }
}

std::string describe_address(uintptr_t addr) {
    const auto module_within = utility::get_module_within(addr);

    if (!module_within) {
        return fmt::format("{:x} (no module)", addr);
    }

    std::string name = "?";

    if (const auto path = utility::get_module_path(*module_within); path) {
        name = std::filesystem::path{*path}.filename().string();
    }

    return fmt::format("{:x} {}+0x{:x}", addr, name, addr - (uintptr_t)*module_within);
}

bool is_probe_module(uintptr_t rip) {
    const auto module_within = utility::get_module_within(rip);

    if (!module_within) {
        return false;
    }

    const auto path = utility::get_module_path(*module_within);

    if (!path) {
        return false;
    }

    auto name = std::filesystem::path{*path}.filename().string();
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return (char)std::tolower(c); });

    // IsBadReadPtr & friends raise real access violations internally; not interesting.
    return name == "kernelbase.dll" || name == "ntdll.dll" || name == "kernel32.dll";
}

bool should_log_rip(uintptr_t rip) {
    for (auto& bucket : g_rip_buckets) {
        auto expected = (uintptr_t)0;

        if (bucket.rip.load(std::memory_order_relaxed) == rip ||
            bucket.rip.compare_exchange_strong(expected, rip, std::memory_order_relaxed)) {
            return bucket.count.fetch_add(1, std::memory_order_relaxed) < FIRST_CHANCE_PER_RIP_CAP;
        }
    }

    // Bucket table full: still log, the global cap bounds us.
    return true;
}

LONG WINAPI first_chance_exception_logger(struct _EXCEPTION_POINTERS* ei) {
    static thread_local bool reentrant = false;

    if (reentrant || ei == nullptr || ei->ExceptionRecord == nullptr || ei->ContextRecord == nullptr) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    const auto code = ei->ExceptionRecord->ExceptionCode;

    if (!is_interesting_exception(code)) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    const auto rip = (uintptr_t)ei->ContextRecord->Rip;

    if (is_probe_module(rip)) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    if (g_first_chance_logged.load(std::memory_order_relaxed) >= FIRST_CHANCE_TOTAL_CAP) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    if (!should_log_rip(rip)) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    reentrant = true;

    try {
        const auto n = g_first_chance_logged.fetch_add(1, std::memory_order_relaxed) + 1;

        std::string detail{};

        if (code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_IN_PAGE_ERROR) {
            const auto kind = ei->ExceptionRecord->ExceptionInformation[0];
            const auto target = ei->ExceptionRecord->ExceptionInformation[1];
            detail = fmt::format(" {} at {:x}",
                kind == 0 ? "read" : (kind == 1 ? "write" : (kind == 8 ? "execute(DEP)" : "access")),
                (uintptr_t)target);
        }

        spdlog::error("[FirstChance #{}] code={:x}{} tid={} rip={}", n, code, detail, GetCurrentThreadId(), describe_address(rip));
        spdlog::error("[FirstChance #{}] rcx={:x} rdx={:x} r8={:x} r9={:x} rax={:x} rbx={:x} rsi={:x} rdi={:x} rbp={:x} rsp={:x}",
            n,
            ei->ContextRecord->Rcx, ei->ContextRecord->Rdx, ei->ContextRecord->R8, ei->ContextRecord->R9,
            ei->ContextRecord->Rax, ei->ContextRecord->Rbx, ei->ContextRecord->Rsi, ei->ContextRecord->Rdi,
            ei->ContextRecord->Rbp, ei->ContextRecord->Rsp);

        if (code != EXCEPTION_STACK_OVERFLOW) {
            // Conservative return-address scan of the faulting stack. The VEH runs on the
            // faulting thread's stack, so a RtlCaptureStackBackTrace here only shows the
            // dispatcher; scanning upward from the faulting RSP finds the real callers.
            const auto rsp = (uintptr_t)ei->ContextRecord->Rsp;
            uint32_t found = 0;

            for (uintptr_t sp = rsp; sp < rsp + 0x800 && found < 16; sp += sizeof(void*)) {
                if (IsBadReadPtr((void*)sp, sizeof(void*))) {
                    break;
                }

                const auto candidate = *(uintptr_t*)sp;

                if (candidate < 0x10000 || IsBadReadPtr((void*)candidate, 1)) {
                    continue;
                }

                if (const auto m = utility::get_module_within(candidate); m) {
                    spdlog::error("[FirstChance #{}]   stack[+{:x}] {}", n, sp - rsp, describe_address(candidate));
                    ++found;
                }
            }
        }

        spdlog::default_logger()->flush();
    } catch (...) {
    }

    reentrant = false;
    return EXCEPTION_CONTINUE_SEARCH;
}
} // namespace

void framework::arm_first_chance_exception_logger(const char* reason) {
    bool expected = false;

    if (!g_first_chance_armed.compare_exchange_strong(expected, true)) {
        return;
    }

    const auto handle = AddVectoredExceptionHandler(1, first_chance_exception_logger);
    spdlog::error("[FirstChance] Armed first-chance exception logger ({}), handle={:x}", reason != nullptr ? reason : "", (uintptr_t)handle);
}

bool framework::is_first_chance_exception_logger_armed() {
    return g_first_chance_armed.load(std::memory_order_relaxed);
}

void framework::enable_log_flush_all(const char* reason) {
    static std::atomic<bool> enabled{false};
    bool expected = false;

    if (!enabled.compare_exchange_strong(expected, true)) {
        return;
    }

    spdlog::flush_on(spdlog::level::trace);
    spdlog::error("[Diagnostics] Log flush-all enabled ({})", reason != nullptr ? reason : "");
}
