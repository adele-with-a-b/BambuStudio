// CutSurfaceHelper -- subprocess-based isolation for cut_surface().
// See CutSurfaceHelper.hpp for the architectural rationale; this file is
// the implementation: wire format + parent-side fork/exec/wait wrapper +
// helper-process entry point.

#include "CutSurfaceHelper.hpp"
#include "ExPolygon.hpp"
#include <admesh/stl.h>          // indexed_triangle_set
#include <boost/log/trivial.hpp>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <vector>
#include <string>
#include <atomic>
#include <chrono>
#include <thread>
#include <mutex>

#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>          // _NSGetExecutablePath
#include <mach/mach.h>            // task_set_exception_ports (helper crash-report suppression)
#include <mach/exc.h>
#include <pthread.h>
#endif
#include <spawn.h>                // posix_spawn

extern char **environ;

#include <Eigen/Geometry>          // Transform3d / Vec3d for projection

namespace Slic3r {

// ============================================================================
// Helper crash-report suppression (macOS Mach exception handler)
// ============================================================================
//
// The helper runs cut_surface() on hostile emoji/glyph input where CGAL's
// Epeck corefine recurses through GMP exact-rational arithmetic and stomps
// the stack (SIGBUS in __gmpn_*). The subprocess design means this never
// crashes the GUI -- but the helper's signal-death still makes macOS write a
// user-visible crash report (.ips) per hostile cut, which is the signal
// upstream QA used to revert the prior 16MB-stack fix
// (bambulab/BambuStudio#10847). A subprocess that spams crash reports is not
// a shippable fix.
//
// We suppress the report at its source: install a task-level Mach exception
// handler that catches EXC_BAD_ACCESS (and bad-instruction / arithmetic) on a
// DEDICATED thread with its own stack -- so it runs even when the primary
// stack has overflowed, which is exactly why this works where the in-process
// sigsetjmp/longjmp recovery did not. On any such exception the handler
// _exit(EXIT_HELPER_OVERFLOW)s before macOS's ReportCrash is invoked: the
// process dies by a chosen exit code, not by an uncaught signal, so no .ips
// is filed. The parent already maps any non-zero exit to an empty cut.
//
// This handles the SIGNAL failure mode. The thrown-C++-exception failure mode
// (CGAL assertion -> __cxa_throw -> std::terminate -> SIGABRT) is handled
// separately by a try/catch around the cut_surface call below. BOTH are
// required: a 30x run on the known-hostile seq_19 input (which crashes an
// unguarded build 10/10 at the worker stack size) produced 0 crash reports
// only with both guards in place; the Mach handler alone left a ~5% SIGABRT
// residual.
// Exit code the helper uses when it catches an unrecoverable CGAL/GMP
// overflow (signal or thrown). Parent treats any non-zero exit as a failed
// cut -> empty SurfaceCut, so this needs no special-casing on the parent side.
namespace { constexpr int EXIT_HELPER_OVERFLOW = 70; }

#if defined(__APPLE__)
namespace {

mach_port_t g_helper_exc_port = MACH_PORT_NULL;

// Mach exception server loop, runs on its own thread (own stack). On ANY
// caught exception message, exit clean -- no reply to the kernel, so the
// process terminates by exit code rather than being handed to ReportCrash.
extern "C" void *helper_exc_thread(void *)
{
    struct { mach_msg_header_t head; char body[1024]; } msg;
    for (;;) {
        kern_return_t kr = mach_msg(&msg.head, MACH_RCV_MSG, 0, sizeof(msg),
                                    g_helper_exc_port, MACH_MSG_TIMEOUT_NONE,
                                    MACH_PORT_NULL);
        if (kr != KERN_SUCCESS)
            continue;
        ::fflush(nullptr);
        ::_exit(EXIT_HELPER_OVERFLOW);
    }
    return nullptr;
}

// Install the task-level exception port + start the server thread. Idempotent
// enough for the helper's single call at startup.
void install_helper_crash_suppression()
{
    if (mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE,
                           &g_helper_exc_port) != KERN_SUCCESS)
        return;
    if (mach_port_insert_right(mach_task_self(), g_helper_exc_port,
                               g_helper_exc_port, MACH_MSG_TYPE_MAKE_SEND) != KERN_SUCCESS)
        return;
    task_set_exception_ports(mach_task_self(),
        EXC_MASK_BAD_ACCESS | EXC_MASK_BAD_INSTRUCTION | EXC_MASK_ARITHMETIC,
        g_helper_exc_port, EXCEPTION_DEFAULT, MACHINE_THREAD_STATE);
    pthread_t t;
    if (pthread_create(&t, nullptr, helper_exc_thread, nullptr) == 0)
        pthread_detach(t);
}

} // anonymous namespace
#else
namespace { inline void install_helper_crash_suppression() {} }
#endif

// ============================================================================
// Wire format
// ============================================================================
//
// Length-prefixed binary, little-endian native (Bambu Studio targets all
// little-endian platforms). NOT human-readable -- this is an internal IPC.
//
// CHANNELS:
//   The helper uses TWO dedicated pipes for protocol traffic, distinct from
//   stdin/stdout/stderr. This isolates the wire protocol from boost::log
//   output (which during libslic3r static init writes a "[trace] Initializing
//   StaticPrintConfigs" line to stdout before main() can suppress it -- if we
//   used stdout for the response, the magic-byte check in the parent would
//   read those bytes first and bail). So:
//     fd 3  -- request: parent writes, helper reads (parent's "in" pipe)
//     fd 4  -- response: helper writes, parent reads (parent's "out" pipe)
//   stdin/stdout/stderr remain wired to the parent's stdin/stdout/stderr so
//   boost::log output and any other prints just flow through to the user's
//   terminal where they're harmless.
//
//   REQUEST (parent -> helper, on helper fd 3):
//     8B   magic = "BSCSH001"
//     ExPolygons        -- write_expolygons / read_expolygons
//     vector<ITS>       -- write_models / read_models
//     uint8 proj_kind   -- 1 = OrthoProject (only kind supported today)
//     16x double matrix (Eigen Transform3d, default storage = column-major)
//     3x  double direction (Vec3d)
//     float projection_ratio
//
//   RESPONSE (helper -> parent, on helper fd 4):
//     8B   magic = "BSCSR001"
//     uint8 status     -- 0 = SurfaceCut payload follows
//                         1 = empty cut (cut_surface returned {} cleanly)
//                         2 = caller-error (e.g. helper got bad input)
//     if status == 0:
//       uint32 n_verts ; float verts[n_verts][3]
//       uint32 n_indices ; int32 indices[n_indices][3]
//       uint32 n_contours
//       for each contour: uint32 n_pts ; uint32 pts[n_pts]
//
// Anything other than a clean status==0/1 response (helper exit != 0,
// helper killed by signal, deadline exceeded, IO error) is mapped by the
// parent to "cut failed; return empty SurfaceCut" -- the same recovery
// behaviour as the existing in-process try_catch_signal path.

namespace {

// --- low-level read/write that handle EINTR + short reads ------------------

static bool full_write(int fd, const void *buf, size_t n) {
    const char *p = static_cast<const char *>(buf);
    while (n > 0) {
        ssize_t k = ::write(fd, p, n);
        if (k < 0) { if (errno == EINTR) continue; return false; }
        if (k == 0) return false;
        p += k; n -= (size_t)k;
    }
    return true;
}
static bool full_read(int fd, void *buf, size_t n) {
    char *p = static_cast<char *>(buf);
    while (n > 0) {
        ssize_t k = ::read(fd, p, n);
        if (k < 0) { if (errno == EINTR) continue; return false; }
        if (k == 0) return false;
        p += k; n -= (size_t)k;
    }
    return true;
}

template<typename T> static bool wr_pod(int fd, const T &v) { return full_write(fd, &v, sizeof(T)); }
template<typename T> static bool rd_pod(int fd, T &v)       { return full_read (fd, &v, sizeof(T)); }
static bool wr_bytes(int fd, const void *p, size_t n) { return full_write(fd, p, n); }
static bool rd_bytes(int fd, void *p, size_t n)       { return full_read (fd, p, n); }

static const char kRequestMagic [8] = {'B','S','C','S','H','0','0','1'};
static const char kResponseMagic[8] = {'B','S','C','S','R','0','0','1'};

enum ProjKind : uint8_t { PROJ_ORTHO = 1 };
enum RespStatus : uint8_t { RESP_OK = 0, RESP_EMPTY = 1, RESP_BAD_INPUT = 2 };

// --- serialize / deserialize -----------------------------------------------

static bool write_polygon(int fd, const Polygon &poly) {
    uint32_t n = (uint32_t)poly.points.size();
    if (!wr_pod(fd, n)) return false;
    for (const Point &pt : poly.points) {
        int32_t xy[2] = { (int32_t)pt.x(), (int32_t)pt.y() };
        if (!wr_bytes(fd, xy, sizeof(xy))) return false;
    }
    return true;
}
static bool read_polygon(int fd, Polygon &poly) {
    uint32_t n; if (!rd_pod(fd, n)) return false;
    poly.points.clear(); poly.points.reserve(n);
    for (uint32_t i = 0; i < n; ++i) {
        int32_t xy[2];
        if (!rd_bytes(fd, xy, sizeof(xy))) return false;
        poly.points.emplace_back(Point{xy[0], xy[1]});
    }
    return true;
}

static bool write_expolygons(int fd, const ExPolygons &es) {
    uint32_t n = (uint32_t)es.size();
    if (!wr_pod(fd, n)) return false;
    for (const ExPolygon &e : es) {
        if (!write_polygon(fd, e.contour)) return false;
        uint32_t h = (uint32_t)e.holes.size();
        if (!wr_pod(fd, h)) return false;
        for (const Polygon &hole : e.holes)
            if (!write_polygon(fd, hole)) return false;
    }
    return true;
}
static bool read_expolygons(int fd, ExPolygons &es) {
    uint32_t n; if (!rd_pod(fd, n)) return false;
    es.clear(); es.reserve(n);
    for (uint32_t i = 0; i < n; ++i) {
        ExPolygon e;
        if (!read_polygon(fd, e.contour)) return false;
        uint32_t h; if (!rd_pod(fd, h)) return false;
        e.holes.resize(h);
        for (uint32_t j = 0; j < h; ++j)
            if (!read_polygon(fd, e.holes[j])) return false;
        es.emplace_back(std::move(e));
    }
    return true;
}

static bool write_its(int fd, const indexed_triangle_set &its) {
    uint32_t nv = (uint32_t)its.vertices.size();
    if (!wr_pod(fd, nv)) return false;
    if (nv) if (!wr_bytes(fd, its.vertices.data(), nv * sizeof(Vec3f))) return false;
    uint32_t ni = (uint32_t)its.indices.size();
    if (!wr_pod(fd, ni)) return false;
    if (ni) if (!wr_bytes(fd, its.indices.data(), ni * sizeof(Vec3i32))) return false;
    return true;
}
static bool read_its(int fd, indexed_triangle_set &its) {
    uint32_t nv; if (!rd_pod(fd, nv)) return false;
    its.vertices.resize(nv);
    if (nv) if (!rd_bytes(fd, its.vertices.data(), nv * sizeof(Vec3f))) return false;
    uint32_t ni; if (!rd_pod(fd, ni)) return false;
    its.indices.resize(ni);
    if (ni) if (!rd_bytes(fd, its.indices.data(), ni * sizeof(Vec3i32))) return false;
    return true;
}

static bool write_models(int fd, const std::vector<indexed_triangle_set> &ms) {
    uint32_t n = (uint32_t)ms.size();
    if (!wr_pod(fd, n)) return false;
    for (const auto &m : ms) if (!write_its(fd, m)) return false;
    return true;
}
static bool read_models(int fd, std::vector<indexed_triangle_set> &ms) {
    uint32_t n; if (!rd_pod(fd, n)) return false;
    ms.resize(n);
    for (uint32_t i = 0; i < n; ++i) if (!read_its(fd, ms[i])) return false;
    return true;
}

// Eigen Transform3d default is column-major and stores 16 doubles internally
// (4x4 affine). We dump the underlying storage directly.
static bool write_transform(int fd, const Transform3d &m) {
    return wr_bytes(fd, m.data(), 16 * sizeof(double));
}
static bool read_transform(int fd, Transform3d &m) {
    return rd_bytes(fd, m.data(), 16 * sizeof(double));
}
static bool write_vec3d(int fd, const Vec3d &v) {
    return wr_bytes(fd, v.data(), 3 * sizeof(double));
}
static bool read_vec3d(int fd, Vec3d &v) {
    return rd_bytes(fd, v.data(), 3 * sizeof(double));
}

static bool write_projection(int fd, const Emboss::IProjection &proj) {
    const auto *ortho = dynamic_cast<const Emboss::OrthoProject *>(&proj);
    if (!ortho) {
        BOOST_LOG_TRIVIAL(error)
            << "[CutSurfaceHelper] write_projection: only OrthoProject is "
               "supported on the wire today (this is the only IProjection "
               "subclass that reaches cut_surface in the emboss flow). "
               "Refusing -- caller will fall back to in-process call.";
        return false;
    }
    uint8_t kind = (uint8_t)PROJ_ORTHO;
    if (!wr_pod(fd, kind)) return false;
    if (!write_transform(fd, ortho->matrix())) return false;
    if (!write_vec3d(fd, ortho->direction())) return false;
    return true;
}

// --- result write/read -----------------------------------------------------

static bool write_surface_cut(int fd, const SurfaceCut &cut) {
    uint32_t nv = (uint32_t)cut.vertices.size();
    if (!wr_pod(fd, nv)) return false;
    if (nv) if (!wr_bytes(fd, cut.vertices.data(), nv * sizeof(Vec3f))) return false;
    uint32_t ni = (uint32_t)cut.indices.size();
    if (!wr_pod(fd, ni)) return false;
    if (ni) if (!wr_bytes(fd, cut.indices.data(), ni * sizeof(Vec3i32))) return false;
    uint32_t nc = (uint32_t)cut.contours.size();
    if (!wr_pod(fd, nc)) return false;
    for (const auto &c : cut.contours) {
        uint32_t np = (uint32_t)c.size();
        if (!wr_pod(fd, np)) return false;
        if (np) if (!wr_bytes(fd, c.data(), np * sizeof(uint32_t))) return false;
    }
    return true;
}
static bool read_surface_cut(int fd, SurfaceCut &cut) {
    uint32_t nv; if (!rd_pod(fd, nv)) return false;
    cut.vertices.resize(nv);
    if (nv) if (!rd_bytes(fd, cut.vertices.data(), nv * sizeof(Vec3f))) return false;
    uint32_t ni; if (!rd_pod(fd, ni)) return false;
    cut.indices.resize(ni);
    if (ni) if (!rd_bytes(fd, cut.indices.data(), ni * sizeof(Vec3i32))) return false;
    uint32_t nc; if (!rd_pod(fd, nc)) return false;
    cut.contours.resize(nc);
    for (uint32_t i = 0; i < nc; ++i) {
        uint32_t np; if (!rd_pod(fd, np)) return false;
        cut.contours[i].resize(np);
        if (np) if (!rd_bytes(fd, cut.contours[i].data(), np * sizeof(uint32_t))) return false;
    }
    return true;
}

// --- locate the path of THIS executable so we can re-exec it ----------------

static std::string self_executable_path() {
#if defined(__APPLE__)
    char buf[4096]; uint32_t sz = sizeof(buf);
    if (_NSGetExecutablePath(buf, &sz) == 0) return std::string(buf);
    return {};
#else
    char buf[4096];
    ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n > 0) { buf[n] = 0; return std::string(buf); }
    return {};
#endif
}

// --- helper-process spawn / wait / kill -------------------------------------

struct HelperPipes {
    int  req_w  = -1;   // parent writes, child reads on fd 3
    int  resp_r = -1;   // child writes on fd 4, parent reads
    pid_t pid   = -1;
};

// Use HIGH fd numbers (>= 50) for protocol channels so they don't collide
// with the low fds (3..6) typically returned by pipe() in a fresh process.
// If we used 3/4, the close-actions for the parent-side pipe ends would
// double as closes of our just-installed protocol fds (observed on macOS:
// pipe() returns 3,4 / 5,6; with target fds 3/4, the addclose(req_pipe[1])
// where req_pipe[1]==4 wipes out the resp protocol fd we just dup2'd).
constexpr int kHelperReqFd  = 50; // child reads request from this fd
constexpr int kHelperRespFd = 51; // child writes response to this fd

// Spawn this same binary with argv[1] = "--cut-surface-helper". Returns true
// on successful spawn; the request-pipe write end and response-pipe read end
// are filled in. On failure, both fds are -1 and pid is -1.
//
// We use DEDICATED fds (3, 4) for the protocol instead of stdin/stdout so
// that any stray prints by libslic3r static init (boost::log writes a
// "[trace] Initializing StaticPrintConfigs" line to stdout before main()
// runs) cannot pollute the protocol stream.
static bool spawn_helper(HelperPipes &out) {
    int req_pipe [2] = {-1,-1};
    int resp_pipe[2] = {-1,-1};
    if (::pipe(req_pipe ) < 0) return false;
    if (::pipe(resp_pipe) < 0) { ::close(req_pipe[0]); ::close(req_pipe[1]); return false; }

    // pipe() typically returns the lowest free fds, which are often 3/4 in
    // a fresh process -- the EXACT fds we want as our protocol channels in
    // the child. If we naively (adddup2(3 -> 3), addclose(3)), the close
    // wipes out our just-installed fd. Sidestep that collision by dup'ing
    // both pipe ends to fresh high fds BEFORE handing them to file_actions.
    int req_read_dup  = ::fcntl(req_pipe[0],  F_DUPFD_CLOEXEC, 10);
    int resp_write_dup = ::fcntl(resp_pipe[1], F_DUPFD_CLOEXEC, 10);
    if (req_read_dup < 0 || resp_write_dup < 0) {
        if (req_read_dup  >= 0) ::close(req_read_dup);
        if (resp_write_dup>= 0) ::close(resp_write_dup);
        ::close(req_pipe[0]);  ::close(req_pipe[1]);
        ::close(resp_pipe[0]); ::close(resp_pipe[1]);
        return false;
    }
    // The originals will be closed by adddup2's effect-on-close-or-by-explicit-close;
    // for clarity we close them now in the parent (the dup'd copies are what's
    // alive in the child after the spawn).
    ::close(req_pipe[0]);  req_pipe[0]  = -1;
    ::close(resp_pipe[1]); resp_pipe[1] = -1;

    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    // Child fd 3 (kHelperReqFd) <- the dup of the request-read end.
    // Child fd 4 (kHelperRespFd) <- the dup of the response-write end.
    posix_spawn_file_actions_adddup2(&fa, req_read_dup,   kHelperReqFd);
    posix_spawn_file_actions_adddup2(&fa, resp_write_dup, kHelperRespFd);
    // After dup2 installs the protocol fds, close the high-numbered duplicates
    // and the parent-side ends of both pipes inside the child. This is safe
    // because adddup2 happens BEFORE addclose in the action sequence.
    posix_spawn_file_actions_addclose(&fa, req_read_dup);
    posix_spawn_file_actions_addclose(&fa, resp_write_dup);
    posix_spawn_file_actions_addclose(&fa, req_pipe[1]);   // parent's write end
    posix_spawn_file_actions_addclose(&fa, resp_pipe[0]);  // parent's read end
    // stdin/stdout/stderr: inherited from parent. boost::log spam is harmless
    // there (and helpful for diagnostics).

    std::string self = self_executable_path();
    if (self.empty()) {
        posix_spawn_file_actions_destroy(&fa);
        ::close(req_read_dup);  ::close(resp_write_dup);
        ::close(req_pipe[1]);   ::close(resp_pipe[0]);
        return false;
    }

    char  arg0[]  = "BambuStudio_helper"; // cosmetic argv0 for ps -ef visibility
    char  arg1[]  = "--cut-surface-helper";
    char *argv[] = { arg0, arg1, nullptr };

    pid_t pid = -1;
    int rc = posix_spawn(&pid, self.c_str(), &fa, nullptr, argv, environ);
    posix_spawn_file_actions_destroy(&fa);

    if (rc != 0 || pid <= 0) {
        ::close(req_read_dup);  ::close(resp_write_dup);
        ::close(req_pipe[1]);   ::close(resp_pipe[0]);
        return false;
    }

    // The dup'd ends were given to the child; close our copies in the parent.
    ::close(req_read_dup);
    ::close(resp_write_dup);
    out.req_w  = req_pipe[1];   // parent's write end of request pipe
    out.resp_r = resp_pipe[0];  // parent's read end of response pipe
    out.pid    = pid;
    return true;
}

// Reap the child. If still running and force_kill=true, SIGKILL it first.
// Returns the exit / signal info as a status integer (Posix convention).
static int reap_helper(HelperPipes &p, bool force_kill, int *out_status) {
    if (p.pid <= 0) return -1;
    if (force_kill) ::kill(p.pid, SIGKILL);
    int status = 0;
    int rc = -1;
    while (true) {
        rc = ::waitpid(p.pid, &status, 0);
        if (rc >= 0) break;
        if (errno == EINTR) continue;
        break;
    }
    if (out_status) *out_status = status;
    return rc;
}

} // namespace

// ============================================================================
// Helper-process entry point
// ============================================================================

int cut_surface_helper_main()
{
    const int kReq  = kHelperReqFd;
    const int kResp = kHelperRespFd;

    // Ignore SIGPIPE: if the parent exits before we finish writing, we'd
    // rather see EPIPE on write() and exit cleanly than be killed by
    // SIGPIPE (which the parent would interpret as the irrecoverable
    // CGAL/GMP stack-stomp signal).
    ::signal(SIGPIPE, SIG_IGN);

    // Suppress macOS crash reports for the expected CGAL/GMP stack-stomp:
    // a Mach exception handler converts the SIGBUS into a clean exit before
    // ReportCrash runs. See install_helper_crash_suppression() above.
    install_helper_crash_suppression();

    // Read magic to confirm the parent is speaking our protocol.
    char mg[8];
    if (!full_read(kReq, mg, sizeof(mg))) return 10;
    if (std::memcmp(mg, kRequestMagic, 8) != 0)   return 11;

    ExPolygons shapes;
    if (!read_expolygons(kReq, shapes)) return 12;

    std::vector<indexed_triangle_set> models;
    if (!read_models(kReq, models))     return 13;

    uint8_t kind = 0;
    if (!rd_pod(kReq, kind))            return 14;
    if (kind != PROJ_ORTHO)             return 15;

    Transform3d matrix; Vec3d direction;
    if (!read_transform(kReq, matrix))  return 16;
    if (!read_vec3d(kReq, direction))   return 17;

    float projection_ratio = 0;
    if (!rd_pod(kReq, projection_ratio)) return 18;

    Emboss::OrthoProject projection(matrix, direction);

    // Two guards around the cut, both required for zero crash reports on
    // hostile input (verified: 30/30 clean on the seq_19 reproducer that
    // crashes an unguarded build 10/10):
    //   1. Mach exception handler (installed above) catches the SIGBUS
    //      stack-stomp -> clean _exit before ReportCrash.
    //   2. try/catch here catches a thrown CGAL assertion (which would
    //      otherwise reach std::terminate -> SIGABRT -> crash report).
    // On either failure the helper exits non-zero; the parent maps that to
    // an empty SurfaceCut.
    SurfaceCut cut;
    try {
        cut = Slic3r::cut_surface(shapes, models, projection, projection_ratio);
    } catch (...) {
        ::fflush(nullptr);
        ::_exit(EXIT_HELPER_OVERFLOW);
    }

    // Write response.
    if (!full_write(kResp, kResponseMagic, 8)) return 20;

    if (cut.empty()) {
        uint8_t st = (uint8_t)RESP_EMPTY;
        if (!wr_pod(kResp, st)) return 21;
        return 0;
    }

    uint8_t st = (uint8_t)RESP_OK;
    if (!wr_pod(kResp, st))     return 22;
    if (!write_surface_cut(kResp, cut)) return 23;
    return 0;
}

int cut_surface_helper_selftest(const char *model_off, const char *shape_off)
{
    // Install the SAME suppression the real helper uses, then run the crash
    // path (corefine on the OFF pair). Verifies the shipped guard, not a
    // separate harness. No crash report should be filed regardless of outcome.
    ::signal(SIGPIPE, SIG_IGN);
    install_helper_crash_suppression();
    try {
        bool ok = Slic3r::corefine_test(model_off, shape_off);
        std::fprintf(stderr, "selftest: corefine returned ok=%d (no stomp this run)\n", (int)ok);
        return ok ? 0 : 1;
    } catch (...) {
        std::fprintf(stderr, "selftest: caught C++ exception -> clean exit %d\n", EXIT_HELPER_OVERFLOW);
        ::fflush(nullptr);
        ::_exit(EXIT_HELPER_OVERFLOW);
    }
}

// ============================================================================
// Parent-side wrapper
// ============================================================================

SurfaceCut cut_surface_via_helper(const ExPolygons                        &shapes,
                                  const std::vector<indexed_triangle_set> &models,
                                  const Emboss::IProjection               &projection,
                                  float                                    projection_ratio,
                                  int                                      deadline_ms)
{
    // Quick early-out (matches the in-process path's behaviour).
    if (models.empty() || shapes.empty()) return {};

    HelperPipes hp;
    if (!spawn_helper(hp)) {
        BOOST_LOG_TRIVIAL(error)
            << "[CutSurfaceHelper] spawn_helper failed (errno=" << errno
            << "). Falling back to in-process cut_surface().";
        return Slic3r::cut_surface(shapes, models, projection, projection_ratio);
    }
    BOOST_LOG_TRIVIAL(info)
        << "[CutSurfaceHelper] spawned helper pid=" << hp.pid
        << " (deadline " << deadline_ms << "ms)";

    // Watchdog: if the helper hasn't produced output in deadline_ms, kill it.
    // Polled atomic, NOT condition_variable -- we observed cv.wait_for racing
    // with siglongjmp on macOS in earlier work (see CutSurface.cpp watchdog
    // notes). The polling overhead is negligible for a per-cut operation.
    std::atomic<bool> done{false};
    std::atomic<bool> killed{false};
    pid_t   helper_pid = hp.pid;
    std::thread watchdog([&done, &killed, helper_pid, deadline_ms] {
        const int tick_ms = 100;
        int waited_ms = 0;
        while (waited_ms < deadline_ms) {
            std::this_thread::sleep_for(std::chrono::milliseconds(tick_ms));
            if (done.load(std::memory_order_acquire)) return;
            waited_ms += tick_ms;
        }
        if (done.load(std::memory_order_acquire)) return;
        BOOST_LOG_TRIVIAL(error)
            << "[CutSurfaceHelper] helper pid=" << helper_pid
            << " exceeded " << deadline_ms << "ms deadline; SIGKILLing.";
        killed.store(true, std::memory_order_release);
        ::kill(helper_pid, SIGKILL);
    });

    // ---- send request --------------------------------------------------
    bool send_ok = full_write(hp.req_w, kRequestMagic, 8)
                && write_expolygons(hp.req_w, shapes)
                && write_models(hp.req_w, models)
                && write_projection(hp.req_w, projection)
                && wr_pod(hp.req_w, projection_ratio);
    // Close request fd so the helper sees EOF (it shouldn't read anything
    // more, but being explicit is safer than relying on a layered call).
    ::close(hp.req_w);
    hp.req_w = -1;

    if (!send_ok) {
        BOOST_LOG_TRIVIAL(error) << "[CutSurfaceHelper] failed to send request to helper";
        ::close(hp.resp_r); hp.resp_r = -1;
        done.store(true, std::memory_order_release);
        watchdog.join();
        int status = 0;
        reap_helper(hp, /*force_kill=*/true, &status);
        // Fall back to in-process call -- send-side IO failures usually mean
        // the helper crashed before reading input, which would mean an
        // in-process call would crash too. But if instead it was a fork()
        // resource issue (pipe broken etc.), the in-process call still works.
        BOOST_LOG_TRIVIAL(error)
            << "[CutSurfaceHelper] send failure; falling back to in-process cut_surface.";
        return Slic3r::cut_surface(shapes, models, projection, projection_ratio);
    }

    // ---- read response -------------------------------------------------
    bool read_ok = false;
    SurfaceCut cut;
    bool empty_response = false;
    {
        char mg[8];
        if (full_read(hp.resp_r, mg, sizeof(mg))
         && std::memcmp(mg, kResponseMagic, 8) == 0) {
            uint8_t st = 255;
            if (rd_pod(hp.resp_r, st)) {
                if (st == RESP_OK) {
                    read_ok = read_surface_cut(hp.resp_r, cut);
                } else if (st == RESP_EMPTY) {
                    read_ok = true;
                    empty_response = true;
                }
            }
        }
    }
    ::close(hp.resp_r); hp.resp_r = -1;
    done.store(true, std::memory_order_release);
    watchdog.join();

    int status = 0;
    reap_helper(hp, /*force_kill=*/false, &status);

    if (killed.load(std::memory_order_acquire)) {
        BOOST_LOG_TRIVIAL(error)
            << "[CutSurfaceHelper] helper killed by deadline; returning empty cut";
        return {};
    }
    if (WIFSIGNALED(status)) {
        BOOST_LOG_TRIVIAL(error)
            << "[CutSurfaceHelper] helper died by signal " << WTERMSIG(status)
            << " -- this is the CGAL/GMP stack-stomp case the helper exists to "
               "isolate. Returning empty cut.";
        return {};
    }
    if (WIFEXITED(status) && WEXITSTATUS(status) != 0) {
        BOOST_LOG_TRIVIAL(error)
            << "[CutSurfaceHelper] helper exited non-zero (code=" << WEXITSTATUS(status)
            << "); returning empty cut.";
        return {};
    }

    if (!read_ok) {
        BOOST_LOG_TRIVIAL(error)
            << "[CutSurfaceHelper] helper exited 0 but response wire was malformed; empty cut.";
        return {};
    }

    if (empty_response) {
        BOOST_LOG_TRIVIAL(info)
            << "[CutSurfaceHelper] helper completed cleanly with empty SurfaceCut "
               "(cut_surface returned {}). Passing through.";
        return {};
    }

    BOOST_LOG_TRIVIAL(info)
        << "[CutSurfaceHelper] helper returned SurfaceCut: "
        << cut.vertices.size() << " verts, "
        << cut.indices.size()  << " tris, "
        << cut.contours.size() << " contours.";
    return cut;
}

} // namespace Slic3r
