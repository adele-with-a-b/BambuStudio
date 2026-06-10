#ifndef TRY_CATCH_SIGNAL_HPP
#define TRY_CATCH_SIGNAL_HPP

#ifdef _MSC_VER
#include "TryCatchSignalSEH.hpp"
#else

#include <csignal>
#include <functional>

namespace Slic3r {

using SignalT = decltype (SIGSEGV);

namespace detail {

// POSIX implementation of try_catch_signal. Installs sigaltstack +
// sigaction for the listed signals, runs fn() under sigsetjmp, and
// invokes cfn() if a listed signal was raised inside fn(). See
// TryCatchSignal.cpp for full semantics + caveats (post-recovery
// state is undefined; the calling worker thread MUST be torn down
// after cfn() returns -- which is the case for PlaterWorker jobs).
void try_catch_signal_posix(int sigcnt, const SignalT *sigs,
                            std::function<void()> &&fn,
                            std::function<void()> &&cfn);

} // namespace detail

// Install a process-wide pthread_introspection hook giving every thread
// (incl. TBB pool threads) a permanent alternate signal stack at creation, so
// the crash handler is always deliverable even on an overflowed stack. Call
// once early in app startup. No-op on Windows / non-Apple.
void install_process_wide_altstacks();

template<class TryFn, class CatchFn, int N>
void try_catch_signal(const SignalT (&sigs)[N], TryFn &&fn, CatchFn &&cfn)
{
    detail::try_catch_signal_posix(N, sigs, fn, cfn);
}

} // namespace Slic3r

#endif

#endif // TRY_CATCH_SIGNAL_HPP

