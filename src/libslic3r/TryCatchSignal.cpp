#include "TryCatchSignal.hpp"

#ifdef _MSC_VER
#include "TryCatchSignalSEH.cpp"
#else

// POSIX (macOS / Linux) implementation of try_catch_signal.
//
// Why this exists: CGAL operations (mesh boolean, surface corefine for
// Emboss text/SVG, etc.) can blow the worker thread's stack on
// near-degenerate input. The recursion is in CGAL's Lazy_exact_nt DAG
// walker plus GMP's mpn_hgcd, both of which are unbounded as a function
// of input precision -- bumping the stack size only delays the failure.
// MeshBoolean.cpp already wraps every CGAL boolean call in
// try_catch_signal({SIGSEGV, SIGFPE, SIGBUS}, ...) for exactly this
// reason, but the POSIX side of the primitive was a no-op stub. This
// implements it.
//
// Mechanism:
//   * A process-wide signal handler is installed once for each signal a
//     caller asks about (SIGSEGV/SIGBUS/SIGFPE), with SA_ONSTACK so the
//     handler runs on a per-thread alternate signal stack -- essential,
//     because the thread's *primary* stack is the thing that overflowed.
//   * Each thread that enters try_catch_signal installs a permanent
//     alternate signal stack (once per thread; never torn down for the
//     life of the thread). A stack overflow on the primary stack can
//     then still deliver the signal onto the alt stack.
//   * try_catch_signal runs fn() under sigsetjmp. If a guarded signal
//     fires while this thread is inside fn(), the handler siglongjmps
//     back and cfn() runs.
//
// Robustness contract (this is the part the narrow first version got
// wrong, and a real-world crash exposed):
//   * The handler is GLOBAL -- it fires on every thread, including ones
//     that are NOT inside a try_catch_signal region, and for stack
//     overflows in code we never wrapped. On those, the per-thread
//     GuardFrame pointer is null. The handler MUST then degrade to a
//     clean default crash and do so WITHOUT consuming stack (the
//     faulting thread may have no alt stack and a fully-overflowed
//     primary stack). We therefore reset the disposition to SIG_DFL and
//     return; the faulting instruction re-executes, re-faults, and the
//     OS produces a normal crash dump. We do not call
//     raise()/sigaction()/printf in that path -- all of those need stack
//     and would turn a clean crash into a confusing secondary fault
//     (observed as EXC_BREAKPOINT/SIGTRAP).
//   * The handler must reach that null check WITHOUT consuming stack
//     either. Its only TLS access is pthread_getspecific(g_guard_key),
//     which on Darwin/arm64 compiles to a stack-free leaf (it reads the
//     thread register directly, no call) and is async-signal-safe in
//     practice -- Apple's own crash reporter relies on it. We
//     deliberately do NOT use C++ thread_local for the handler's state:
//     on macOS/arm64 a thread_local READ compiles to a CALL into the TLS
//     resolver (tlv_get_addr), which needs stack. The
//     __attribute__((tls_model("initial-exec"))) relaxation that would
//     turn that into a direct load on ELF is SILENTLY IGNORED in Mach-O
//     (TLV has no initial-exec form), so the resolver call remained; on
//     a thread with an overflowed primary stack and no alt stack it
//     re-faulted at handler entry, and the handler died at its first
//     instruction. The GuardFrame lives on the guarded function's stack
//     and the handler reads its fields with plain memory loads (the
//     frame is below the overflow point but still mapped), no call.
//   * SA_NODEFER is NOT set. With the signal auto-blocked across the
//     handler, a re-fault inside the handler (the no-alt-stack case
//     above) makes the kernel reset to SIG_DFL and terminate cleanly
//     with the original signal, rather than recursing the handler.
//     Together these two changes guarantee clean degradation on threads
//     that never installed an alt stack. NOTE: such unguarded-thread
//     overflows are NOT recovered (there is no jmp_buf to return to) --
//     they degrade to a clean, attributable crash. Recovery still only
//     happens on threads actually inside a try_catch_signal region.
//
// Caveats (documented, justified, not swept under the rug):
//   1. On siglongjmp recovery, C++ destructors between the faulting
//      frame and the sigsetjmp frame do NOT run. Partially-built CGAL
//      Surface_mesh objects and in-flight GMP mpq temporaries leak.
//      This is a bounded, single-digit-MB per-incident leak. The
//      alternative is a hard app crash, so the tradeoff is clearly
//      worth it.
//   2. GMP here uses its DEFAULT allocator (malloc/free) -- BambuStudio
//      never calls mp_set_memory_functions. There is no GMP-private
//      global lock that could be left held across the longjmp. The
//      system malloc lock is only held transiently inside malloc(); the
//      overflow faults during computation (mpn_mul_1 / mpz_gcd), not
//      inside malloc, so the allocator is not left locked. This is why
//      the calling worker thread stays healthy enough to serve the next
//      job (BoostThreadWorker is a persistent worker, not one-shot).
//   3. sigsetjmp/siglongjmp save/restore the signal mask (the "1" arg
//      to sigsetjmp). Skipping it would leave the just-delivered signal
//      blocked after recovery and silently break the next attempt.

#include <cstdint>
#include <atomic>
#include <cstring>
#include <functional>
#include <mutex>
#include <pthread.h>
#if defined(__APPLE__)
#include <pthread/introspection.h>
#endif
#include <setjmp.h>
#include <signal.h>
#include <sys/mman.h>
#include <unistd.h>

namespace Slic3r {
namespace detail {

namespace {

// Per-thread guard state, reachable from the signal handler. We do NOT use
// C++ thread_local for this: on macOS/arm64 a thread_local READ compiles to a
// CALL into the TLS resolver (tlv_get_addr), which needs stack and is not
// async-signal-safe. The handler may run on a thread whose primary stack just
// overflowed and which has no alt stack, so that call re-faults at handler
// entry. pthread_getspecific compiles to a stack-free leaf (reads the thread
// register directly) and is async-signal-safe in practice, so the handler
// uses it instead. The GuardFrame itself lives on the guarded function's stack
// (try_catch_signal_posix); we store its address in the key.
//
// VALIDATION (this is the part a real crash exposed -- core.1918): the handler
// MUST NOT trust the pthread_getspecific result as a dereferenceable pointer
// after only a null check. Observed failure: the key returned a non-null
// GARBAGE value (0x766e9b5ada2069b0) on a faulting thread whose slot did not
// hold a live GuardFrame; the handler's null check passed and the subsequent
// `ldr x0,[x0]` (read gf->jmp_buf) faulted INSIDE the handler, re-entering it
// and crashing. The fix: every GuardFrame carries a magic sentinel and records
// the [lo,hi) bounds of the stack it lives on. Before touching jmp_buf the
// handler verifies (a) the pointer lies within plausibly-readable space, and
// (b) gf->magic == kGuardMagic. A garbage or dangling pointer fails the magic
// check and the handler degrades to SIG_DFL instead of dereferencing it.
constexpr uint64_t kGuardMagic = 0x5331475541524421ULL; // "!$RAUGS3" - ad hoc

struct GuardFrame {
    uint64_t       magic        = kGuardMagic; // FIRST field: validated by handler
    sigjmp_buf    *jmp_buf      = nullptr;
    int            signal_count = 0;
    const SignalT *signals      = nullptr;
    uintptr_t      stack_lo     = 0; // owning thread's stack low bound
    uintptr_t      stack_hi     = 0; // owning thread's stack high bound
};

pthread_key_t  g_guard_key;
std::once_flag g_guard_key_once;

void ensure_guard_key()
{
    std::call_once(g_guard_key_once, [] { pthread_key_create(&g_guard_key, nullptr); });
}

// Per-thread permanent alternate signal stack. Installed once the first
// time a thread enters try_catch_signal; never torn down (lives for the
// thread's lifetime). Kept in a thread_local holder so it is freed when
// the thread exits.
struct AltStackHolder {
    void  *mem  = nullptr;
    size_t size = 0;
    ~AltStackHolder() {
        if (mem) {
            // Disable the alt stack before unmapping so the kernel does
            // not keep a dangling reference.
            stack_t disable{};
            disable.ss_flags = SS_DISABLE;
            sigaltstack(&disable, nullptr);
            ::munmap(mem, size);
        }
    }
};
thread_local AltStackHolder t_altstack;
thread_local bool           t_altstack_ready = false;

// Process-wide install bookkeeping. One entry per signal number.
constexpr int kMaxSig = 64;
bool           g_installed[kMaxSig] = {false};
std::mutex     g_install_mutex;

// Returns true iff [p, p+len) is safe to read on this thread without faulting.
// async-signal-safe: a single mincore() syscall, no locks, no allocation.
// mincore reports residency per page for MAPPED pages and returns -1/ENOMEM for
// unmapped ranges -- exactly the discriminator we need to avoid dereferencing a
// garbage pointer inside the handler.
static bool addr_readable(const void *p, size_t len)
{
    if (p == nullptr) return false;
    uintptr_t a = reinterpret_cast<uintptr_t>(p);
    if (a < 0x1000) return false; // null page region
    long pg = ::sysconf(_SC_PAGESIZE);
    if (pg <= 0) pg = 4096;
    uintptr_t start = a & ~(uintptr_t)(pg - 1);
    uintptr_t end   = (a + len + pg - 1) & ~(uintptr_t)(pg - 1);
    unsigned char vec[16];
    size_t npages = (end - start) / (size_t)pg;
    if (npages == 0 || npages > sizeof(vec)) return false;
    // mincore: 0 => all pages in range are mapped; -1 (ENOMEM) => a hole.
    return ::mincore(reinterpret_cast<void *>(start), end - start,
                     reinterpret_cast<char *>(vec)) == 0;
}

// The global signal handler. Async-signal-safety: TLS access is
// pthread_getspecific (a stack-free leaf on Darwin); the GuardFrame pointer is
// VALIDATED (mincore + magic + stack-range) before any dereference; otherwise
// we siglongjmp or signal()+return. No malloc, no printf, no thread_local, no
// non-reentrant libc on the hot path.
extern "C" void try_catch_signal_handler(int signum, siginfo_t *, void *)
{
    const GuardFrame *gf = static_cast<const GuardFrame *>(pthread_getspecific(g_guard_key));

    // VALIDATE before any dereference. A non-null garbage/dangling pointer
    // (observed in core.1918: 0x766e9b5ada2069b0) must NOT be dereferenced --
    // doing so faults inside the handler and re-enters it. Check the pointer is
    // mapped, then verify the magic sentinel, then the self-consistency of the
    // recorded stack bounds and that the GuardFrame actually lives within them.
    if (gf != nullptr &&
        addr_readable(gf, sizeof(GuardFrame)) &&
        gf->magic == kGuardMagic &&
        gf->stack_lo != 0 && gf->stack_hi > gf->stack_lo &&
        reinterpret_cast<uintptr_t>(gf) >= gf->stack_lo &&
        reinterpret_cast<uintptr_t>(gf) <  gf->stack_hi &&
        gf->jmp_buf != nullptr && gf->signals != nullptr &&
        gf->signal_count > 0 && gf->signal_count <= 8 &&
        addr_readable(gf->signals, sizeof(SignalT) * (size_t)gf->signal_count)) {
        for (int i = 0; i < gf->signal_count; ++i) {
            if (gf->signals[i] == signum) {
                // Inside a guarded region on this thread for a signal we were
                // asked to catch. Jump back to the sigsetjmp point. Runs on the
                // alt stack (SA_ONSTACK + guarded threads always installed one).
                siglongjmp(*gf->jmp_buf, signum);
                // not reached
            }
        }
    }
    // Not our region (or not our signal, or the guard pointer didn't validate).
    // Degrade to a clean default crash: reset disposition to SIG_DFL and return;
    // the faulting instruction re-executes, re-faults, and (with SA_NODEFER
    // unset, so the signal is blocked across the handler) the kernel terminates
    // cleanly with the ORIGINAL signal. No raise(): on a fully-overflowed stack
    // it could fault and produce a confusing secondary signal.
    signal(signum, SIG_DFL);
}

void ensure_handler_installed_for(int sig)
{
    if (sig <= 0 || sig >= kMaxSig) return;
    std::lock_guard<std::mutex> lk(g_install_mutex);
    if (g_installed[sig]) return;
    struct sigaction sa{};
    sa.sa_sigaction = &try_catch_signal_handler;
    sigemptyset(&sa.sa_mask);
    // SA_ONSTACK  -- run the handler on the per-thread alt stack.
    // SA_SIGINFO  -- three-arg handler form.
    //
    // SA_NODEFER is deliberately NOT set. Without it, the kernel
    // auto-blocks this signal for the duration of the handler. If the
    // handler itself faults (e.g. on a thread with no alt stack whose
    // primary stack just overflowed), the re-fault of a now-blocked
    // synchronous fatal signal forces the kernel to reset the
    // disposition to SIG_DFL and terminate the process cleanly with the
    // ORIGINAL signal -- instead of recursively re-entering the handler
    // and dying with a confusing SIGILL. The old justification for
    // SA_NODEFER ("blocking would leak a masked signal") does NOT hold:
    // the longjmp path uses sigsetjmp/siglongjmp with savemask=1, which
    // restores the pre-signal mask (unblocking the signal) on recovery;
    // and the reset-and-return path unblocks via the normal sigreturn
    // mask restore. Removing SA_NODEFER is therefore safe on both paths
    // and fixes the recursive-handler crash.
    sa.sa_flags = SA_ONSTACK | SA_SIGINFO;
    if (sigaction(sig, &sa, nullptr) == 0)
        g_installed[sig] = true;
}

// Install a permanent alternate signal stack on the current thread if
// not already present. Idempotent per thread.
void ensure_thread_altstack()
{
    if (t_altstack_ready) return;

    // Size: at least 64 KB, and at least SIGSTKSZ (which is runtime-
    // dynamic on macOS 11+ / glibc 2.34+). Enough for siglongjmp plus a
    // shallow handler frame.
    size_t want = (size_t) SIGSTKSZ;
    if (want < (size_t)(64 * 1024)) want = (size_t)(64 * 1024);

    // If this thread already has an alt stack (e.g. installed by some
    // other library), leave it alone and consider ourselves ready.
    stack_t cur{};
    if (sigaltstack(nullptr, &cur) == 0 && !(cur.ss_flags & SS_DISABLE) &&
        cur.ss_sp != nullptr && cur.ss_size >= want) {
        t_altstack_ready = true;
        return;
    }

    void *mem = ::mmap(nullptr, want, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANON, -1, 0);
    if (mem == MAP_FAILED)
        return; // leave t_altstack_ready false; guard degrades safely

    stack_t ss{};
    ss.ss_sp = mem;
    ss.ss_size = want;
    ss.ss_flags = 0;
    if (sigaltstack(&ss, nullptr) != 0) {
        ::munmap(mem, want);
        return;
    }
    t_altstack.mem  = mem;
    t_altstack.size = want;
    t_altstack_ready = true;
}

// --- Process-wide alt-stack coverage via pthread_introspection ------------
//
// The per-thread alt stack above is installed only when a thread enters
// try_catch_signal. But CGAL Epeck/GMP can overflow the stack on threads that
// never enter a guard -- notably TBB worker-pool threads running slicing
// geometry (Arachne / bridge / MMU). On such a thread the overflow delivers a
// signal that SA_ONSTACK cannot place (no alt stack) -> the kernel kills the
// process with the handler never running (observed: raw __gmpz_gcd 2-frame
// crash, no handler frame). To make the handler ALWAYS deliverable, we install
// a permanent alt stack on EVERY thread at creation via a pthread_introspection
// hook -- this fires for TBB threads too, since they are still created through
// libpthread. We chain any previously-installed hook.
#if defined(__APPLE__)
std::atomic<bool>            g_introspect_installed{false};
pthread_introspection_hook_t g_prev_introspection = nullptr;

extern "C" void tcs_introspection_hook(unsigned int event, pthread_t thread,
                                       void *addr, size_t size)
{
    // On THREAD_START the hook runs ON the newly-started thread, so calling
    // ensure_thread_altstack() here installs the alt stack onto that thread --
    // including TBB worker-pool threads, which are still created via libpthread.
    if (event == PTHREAD_INTROSPECTION_THREAD_START)
        ensure_thread_altstack();
    // Chain any previously-installed hook so we don't clobber other libraries.
    if (g_prev_introspection)
        g_prev_introspection(event, thread, addr, size);
}
#endif

// Entry point used by Slic3r::install_process_wide_altstacks() (defined below,
// outside the detail namespace, to match the public declaration).
void install_process_wide_altstacks_impl()
{
#if defined(__APPLE__)
    if (g_introspect_installed.exchange(true)) return;
    ensure_guard_key();
    // Install the SIGSEGV/SIGBUS/SIGFPE handlers UP FRONT, process-wide. These
    // were previously installed lazily by the first try_catch_signal() call --
    // which meant a stack overflow on a thread that ran BEFORE any guarded
    // region hit the DEFAULT disposition (terminate) with no handler frame
    // (observed: raw __gmpn_* crash, no try_catch_signal_handler on the stack).
    // Arming them here, before any worker thread is created, guarantees the
    // handler is always the installed disposition for these signals.
    ensure_handler_installed_for(SIGSEGV);
    ensure_handler_installed_for(SIGBUS);
    ensure_handler_installed_for(SIGFPE);
    g_prev_introspection = pthread_introspection_hook_install(&tcs_introspection_hook);
    ensure_thread_altstack(); // cover the calling (main) thread now
#endif
}

} // anonymous namespace

void try_catch_signal_posix(int sigcnt, const SignalT *sigs,
                            std::function<void()> &&fn,
                            std::function<void()> &&cfn)
{
    if (sigcnt <= 0 || sigs == nullptr) {
        fn();
        return;
    }

    // Ensure the pthread key exists before installing any handler our signals
    // could invoke, then install global handlers (idempotent) and a permanent
    // alt stack on this thread (idempotent).
    ensure_guard_key();
    for (int i = 0; i < sigcnt; ++i)
        ensure_handler_installed_for(sigs[i]);
    ensure_thread_altstack();

    // Save any outer guard pointer (supports nesting) and install ours. The
    // GuardFrame lives on this function's stack; its address goes in the key.
    void *saved_outer = pthread_getspecific(g_guard_key);

    sigjmp_buf jb;
    GuardFrame gf_local;            // magic is set by the default member init
    gf_local.jmp_buf      = &jb;
    gf_local.signal_count = sigcnt;
    gf_local.signals      = sigs;
    // Record THIS thread's stack bounds so the handler can verify the
    // GuardFrame pointer actually points into this stack before dereferencing
    // it (defends against a garbage/dangling key value -- see handler + the
    // core.1918 analysis).
    {
        pthread_t self = pthread_self();
        void  *hi = pthread_get_stackaddr_np(self);   // highest address
        size_t sz = pthread_get_stacksize_np(self);
        gf_local.stack_hi = reinterpret_cast<uintptr_t>(hi);
        gf_local.stack_lo = gf_local.stack_hi > sz ? gf_local.stack_hi - sz : 0;
    }
    pthread_setspecific(g_guard_key, &gf_local);

    // sigsetjmp(jb, 1): the 1 saves/restores the signal mask across the
    // longjmp so the just-caught signal isn't left blocked.
    int signum = sigsetjmp(jb, 1);
    if (signum == 0) {
        try {
            fn();
        } catch (...) {
            pthread_setspecific(g_guard_key, saved_outer);
            throw;
        }
    } else {
        // Recovered from a guarded signal. CGAL/GMP state is undefined;
        // cfn() must do nothing more than record a flag for the caller
        // to surface as a non-fatal error (see CutSurface.cpp).
        cfn();
    }

    // Restore outer guard pointer on every path.
    pthread_setspecific(g_guard_key, saved_outer);
}

} // namespace detail

// Install a process-wide pthread_introspection hook that gives EVERY thread
// (including TBB pool threads we never create ourselves) a permanent alternate
// signal stack at creation. This guarantees the SIGSEGV/SIGBUS/SIGFPE handler
// is always deliverable -- even on an overflowed primary stack -- so a CGAL/GMP
// stack overflow on an unguarded slicing worker degrades to a clean,
// attributable crash (or recovers, where a try_catch_signal region exists)
// instead of an undeliverable-signal hard kill. Call once at startup.
void install_process_wide_altstacks()
{
    detail::install_process_wide_altstacks_impl();
}

} // namespace Slic3r

#endif // !_MSC_VER
