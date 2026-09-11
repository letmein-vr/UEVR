#include <windows.h>

namespace framework {
LONG WINAPI global_exception_handler(struct _EXCEPTION_POINTERS* ei);
void setup_exception_handler();

// Diagnostics: log every first-chance hardware exception (AV, illegal instruction,
// stack overflow, ...) with module+offset and a short backtrace, then continue
// the search so the game's own SEH handling is unaffected. Idempotent.
void arm_first_chance_exception_logger(const char* reason);
bool is_first_chance_exception_logger_armed();

// Diagnostics: flush the log after every message (not just errors) so the log
// tail is trustworthy when the process dies without reaching our handlers.
void enable_log_flush_all(const char* reason);
}
