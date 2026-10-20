#include <windows.h>

namespace framework {
LONG WINAPI global_exception_handler(struct _EXCEPTION_POINTERS* ei);
void setup_exception_handler();

// DllMain calls this on DLL_PROCESS_DETACH, before the C runtime destroys this
// module's static objects. From then on the handlers that stay registered for
// the life of the process (the unhandled exception filter and the vectored
// handlers) pass every exception on untouched: the objects they use may be gone.
void on_module_detach();
bool is_module_detaching();

// Hang diagnostics: suspends every other thread in turn, and logs where each
// one is (RIP as module+offset, plus the module-resident values still on its
// stack). Called from the no-present watchdog in Framework; rate-limited and
// capped internally, so it is safe to call from a polling loop.
void dump_all_thread_stacks(const char* reason);
}
