#include <windows.h>

namespace framework {
LONG WINAPI global_exception_handler(struct _EXCEPTION_POINTERS* ei);
void setup_exception_handler();

// Hang diagnostics: suspends every other thread in turn, and logs where each
// one is (RIP as module+offset, plus the module-resident values still on its
// stack). Called from the no-present watchdog in Framework; rate-limited and
// capped internally, so it is safe to call from a polling loop.
void dump_all_thread_stacks(const char* reason);
}
