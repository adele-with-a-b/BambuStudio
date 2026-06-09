#include <catch2/catch.hpp>
#include <test_utils.hpp>

#include <libslic3r/TriangleMesh.hpp>
#include <libslic3r/MeshBoolean.hpp>

using namespace Slic3r;

TEST_CASE("CGAL and TriangleMesh conversions", "[MeshBoolean]") {
    TriangleMesh sphere = make_sphere(1.);

    auto cgalmesh_ptr = MeshBoolean::cgal::triangle_mesh_to_cgal(sphere);

    REQUIRE(cgalmesh_ptr);
    REQUIRE(! MeshBoolean::cgal::does_self_intersect(*cgalmesh_ptr));

    TriangleMesh M = MeshBoolean::cgal::cgal_to_triangle_mesh(*cgalmesh_ptr);

    REQUIRE(M.its.vertices.size() == sphere.its.vertices.size());
    REQUIRE(M.its.indices.size() == sphere.its.indices.size());

    REQUIRE(M.volume() == Approx(sphere.volume()));

    REQUIRE(! MeshBoolean::cgal::does_self_intersect(M));
}

// ===========================================================================
// GMP stack-overflow reproducer harness  (investigation: fix/lan-stale-mqtt-and-wake)
//
// Goal: deterministically drive Slic3r::MeshBoolean::* deep into GMP exact-
// rational GCD recursion (__gmpz_gcd / __gmpq_div / __gmpn_*) until the worker
// stack (4 MB, == TBB worker == Slic3r::create_thread) overflows, matching the
// production crash core.81612 whose live chain was:
//   gcd_hook / __gmpn_gcd / __gmpz_mul / __gmpq_div
//     <- CGAL::Vector_3<Simple_cartesian<Gmpq>>::Vector_3   (exact rational ctor)
//     <- CGAL::Filtered_predicate<...Collinear_are_strictly_ordered_along_line_3<Epeck>...>
//
// The leading hypothesis (H1) is the top-level
//   MeshBoolean::minus(TriangleMesh&) / self_union(TriangleMesh&)
// which call igl::copyleft::cgal::mesh_boolean<Epeck> (exact 3D kernel -> Gmpq).
//
// All reproducer cases are tagged [!hide] so they DO NOT run in a normal test
// pass (they are designed to crash the process). Run one explicitly, e.g.:
//   ./libslic3r_tests "H1a near-coincident cubes minus" -s
// Each case runs the candidate on a 4 MB boost thread (Slic3r::create_thread)
// and installs a sigaltstack SIGSEGV/SIGBUS backtrace handler so the crash site
// is captured even when the main stack is the one that overflowed.
// ===========================================================================

#include <csignal>
#include <csetjmp>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <atomic>
#include <vector>
#include <string>

#include <unistd.h>
#include <fcntl.h>
#include <execinfo.h>

#include <libslic3r/Thread.hpp>
#include <boost/thread.hpp>

namespace {

// ---------------------------------------------------------------------------
// Crash-site capture: sigaltstack handler that dumps backtrace() async-signal-
// safely to STDERR (write(2)) and to a file under the investigation dir, then
// _exit()s with a distinctive code so the harness/log can tell a GMP overflow
// crash from a clean failure.
// ---------------------------------------------------------------------------
constexpr int kCrashExitCode = 86;  // distinctive: "overflowed"
const char *g_bt_path = "/Users/adele_with_a_b/3d-projects/bambustudio-investigation/last_crash_backtrace.txt";

void crash_handler(int sig, siginfo_t *info, void * /*uctx*/)
{
    // async-signal-safe-ish dump. backtrace()/backtrace_symbols_fd are
    // documented async-signal-safe on Darwin.
    const char *name =
        sig == SIGSEGV ? "SIGSEGV" :
        sig == SIGBUS  ? "SIGBUS"  :
        sig == SIGFPE  ? "SIGFPE"  : "SIG?";

    char hdr[256];
    int n = snprintf(hdr, sizeof(hdr),
                     "\n==== CRASH: %s (signal %d) at fault addr %p ====\n",
                     name, sig, info ? info->si_addr : nullptr);
    if (n > 0) { ssize_t w = write(STDERR_FILENO, hdr, (size_t)n); (void)w; }

    void *frames[256];
    int nframes = backtrace(frames, 256);
    backtrace_symbols_fd(frames, nframes, STDERR_FILENO);

    // also persist to a file for offline inspection
    int fd = open(g_bt_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        ssize_t w = write(fd, hdr, (size_t)(n > 0 ? n : 0)); (void)w;
        backtrace_symbols_fd(frames, nframes, fd);
        close(fd);
    }

    const char *tail = "==== END CRASH BACKTRACE ====\n";
    ssize_t w2 = write(STDERR_FILENO, tail, strlen(tail)); (void)w2;
    _exit(kCrashExitCode);
}

void install_crash_handler()
{
    // Alternate signal stack so the handler can run even when the normal
    // stack overflowed (the whole point).
    static std::vector<char> altstack(SIGSTKSZ > 0 ? SIGSTKSZ * 4 : 256 * 1024);
    stack_t ss{};
    ss.ss_sp = altstack.data();
    ss.ss_size = altstack.size();
    ss.ss_flags = 0;
    sigaltstack(&ss, nullptr);

    struct sigaction sa{};
    sa.sa_sigaction = crash_handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, nullptr);
    sigaction(SIGBUS, &sa, nullptr);
    sigaction(SIGFPE, &sa, nullptr);
}

// Run `fn` on a 4 MB boost thread (same stack budget as TBB workers and
// PlaterWorker jobs). Returns when the thread joins. If fn crashes, the
// crash_handler _exit()s the whole process before this returns.
template <class Fn>
void run_on_worker_stack(Fn &&fn)
{
    install_crash_handler();
    std::exception_ptr eptr;
    boost::thread t = Slic3r::create_thread([&] {
        // Re-install on this thread too: sigaltstack is per-thread on macOS.
        install_crash_handler();
        try {
            fn();
        } catch (...) {
            eptr = std::current_exception();
        }
    });
    t.join();
    if (eptr) std::rethrow_exception(eptr);
}

// ---------------------------------------------------------------------------
// Mesh builders for degenerate / large-coordinate inputs.
// ---------------------------------------------------------------------------

// Translate a TriangleMesh in place by (dx,dy,dz).
void translate(TriangleMesh &m, double dx, double dy, double dz)
{
    for (auto &v : m.its.vertices) {
        v.x() += float(dx);
        v.y() += float(dy);
        v.z() += float(dz);
    }
}

// Scale a TriangleMesh in place.
void scale(TriangleMesh &m, double s)
{
    for (auto &v : m.its.vertices) v *= float(s);
}

// Build a high-tessellation cube-like box by subdividing each face into a
// grid of n*n quads (-> 2*n*n triangles per face). All coplanar -> lots of
// near-collinear/coplanar predicate evaluations during corefinement.
TriangleMesh make_dense_box(double sx, double sy, double sz, int n)
{
    indexed_triangle_set its;
    auto add_quad = [&](Vec3f a, Vec3f b, Vec3f c, Vec3f d) {
        int base = (int)its.vertices.size();
        its.vertices.push_back(a);
        its.vertices.push_back(b);
        its.vertices.push_back(c);
        its.vertices.push_back(d);
        its.indices.emplace_back(base + 0, base + 1, base + 2);
        its.indices.emplace_back(base + 0, base + 2, base + 3);
    };
    auto grid_face = [&](int axis, bool high) {
        for (int i = 0; i < n; ++i) {
            for (int j = 0; j < n; ++j) {
                double u0 = double(i) / n, u1 = double(i + 1) / n;
                double v0 = double(j) / n, v1 = double(j + 1) / n;
                Vec3f a, b, c, d;
                auto mk = [&](double u, double v) -> Vec3f {
                    double cc = high ? 1.0 : 0.0;
                    if (axis == 0) return Vec3f(float(cc * sx), float(u * sy), float(v * sz));
                    if (axis == 1) return Vec3f(float(u * sx), float(cc * sy), float(v * sz));
                    return Vec3f(float(u * sx), float(v * sy), float(cc * sz));
                };
                a = mk(u0, v0); b = mk(u1, v0); c = mk(u1, v1); d = mk(u0, v1);
                add_quad(a, b, c, d);
            }
        }
    };
    for (int axis = 0; axis < 3; ++axis) { grid_face(axis, false); grid_face(axis, true); }
    return TriangleMesh{std::move(its)};
}

} // namespace

// ===========================================================================
// H1 candidates: top-level MeshBoolean (igl Epeck mesh_boolean)
// ===========================================================================

// H1a: two axis-aligned unit cubes overlapping by a sub-epsilon amount. The
// near-coincident faces force the exact kernel to construct intersections that
// reduce to huge rationals.
TEST_CASE("H1a near-coincident cubes minus", "[MeshBoolean][!hide][repro]")
{
    run_on_worker_stack([] {
        TriangleMesh a = make_cube(10., 10., 10.);
        TriangleMesh b = make_cube(10., 10., 10.);
        // overlap b into a by an amount far below float epsilon at this scale,
        // so the cut plane is "almost" coincident with a's face.
        translate(b, 10.0 - 1e-7, 0.0, 0.0);
        MeshBoolean::minus(a, b);  // igl Epeck mesh_boolean
        printf("H1a survived: result verts=%zu\n", a.its.vertices.size());
    });
}

// H1b: large-coordinate cubes (Slic3r coord scale ~1e5; verts at ~1e7) so the
// Epeck numerators/denominators explode during exact construction.
TEST_CASE("H1b large-coordinate cubes minus", "[MeshBoolean][!hide][repro]")
{
    run_on_worker_stack([] {
        TriangleMesh a = make_cube(1., 1., 1.);
        TriangleMesh b = make_cube(1., 1., 1.);
        scale(a, 1e7);
        scale(b, 1e7);
        translate(b, 0.5e7 + 0.123456, 0.5e7 + 0.987654, 0.0);
        MeshBoolean::minus(a, b);
        printf("H1b survived: result verts=%zu\n", a.its.vertices.size());
    });
}

// H1c: dense coplanar grid boxes sharing a near-coincident face. Many near-
// collinear coplanar vertices -> exact predicate storm.
TEST_CASE("H1c dense coplanar grid boxes minus", "[MeshBoolean][!hide][repro]")
{
    run_on_worker_stack([] {
        TriangleMesh a = make_dense_box(20., 20., 20., 24);
        TriangleMesh b = make_dense_box(20., 20., 20., 24);
        translate(b, 20.0 - 5e-6, 1e-6, 1e-6);  // faces nearly coincident & slightly skewed
        MeshBoolean::minus(a, b);
        printf("H1c survived: result verts=%zu\n", a.its.vertices.size());
    });
}

// H1d: self_union of a self-intersecting / coincident-face soup (two cubes
// fused at a shared face, fed as one mesh) -> the self_union Epeck path.
TEST_CASE("H1d self_union coincident-face soup", "[MeshBoolean][!hide][repro]")
{
    run_on_worker_stack([] {
        TriangleMesh a = make_cube(10., 10., 10.);
        TriangleMesh b = make_cube(10., 10., 10.);
        translate(b, 10.0 - 1e-7, 1e-7, 0.0);
        // merge into one soup mesh
        int base = (int)a.its.vertices.size();
        for (auto &v : b.its.vertices) a.its.vertices.push_back(v);
        for (auto &f : b.its.indices)
            a.its.indices.emplace_back(f.x() + base, f.y() + base, f.z() + base);
        MeshBoolean::self_union(a);
        printf("H1d survived: result verts=%zu\n", a.its.vertices.size());
    });
}

// ===========================================================================
// H2 candidates: PRODUCTION corefinement path
//   MeshBoolean::cgal::{minus,plus,intersect}(TriangleMesh&)
//   -> corefine_and_compute_* on Epick Surface_mesh, BUT corefinement still
//      constructs EXACT intersection points (Filtered_kernel exact fallback ->
//      Gmpq) when the double filter fails on near-degenerate input. This is the
//      path the GUI's CSG/part boolean (PerformCSGMeshBooleans.hpp) actually
//      calls. This path is ALREADY wrapped in try_catch_signal {SEGV,FPE,BUS}
//      inside MeshBoolean.cpp (_cgal_do), so on a thread where the guard works it
//      throws Slic3r::HardCrash instead of overflowing. A thrown HardCrash here is
//      itself proof that the corefine path hit the GMP overflow. If the process
//      instead dies with our SIGSEGV/SIGBUS backtrace handler, the guard failed
//      to cover this thread -- which is precisely the production bug.
// ===========================================================================

#include <libslic3r/Exception.hpp>

// H2a: production corefine difference, near-coincident faces.
TEST_CASE("H2a cgal::minus near-coincident cubes", "[MeshBoolean][!hide][repro]")
{
    run_on_worker_stack([] {
        TriangleMesh a = make_cube(10., 10., 10.);
        TriangleMesh b = make_cube(10., 10., 10.);
        translate(b, 10.0 - 1e-7, 0.0, 0.0);
        try {
            MeshBoolean::cgal::minus(a, b);
            printf("H2a survived (no crash): result verts=%zu\n", a.its.vertices.size());
        } catch (const Slic3r::HardCrash &e) {
            printf("H2a HARDCRASH (guard caught GMP overflow in corefine): %s\n", e.what());
        } catch (const std::exception &e) {
            printf("H2a threw std::exception: %s\n", e.what());
        }
    });
}

// H2b: production corefine union of coincident-face soup.
TEST_CASE("H2b cgal::plus coincident-face cubes", "[MeshBoolean][!hide][repro]")
{
    run_on_worker_stack([] {
        TriangleMesh a = make_cube(10., 10., 10.);
        TriangleMesh b = make_cube(10., 10., 10.);
        translate(b, 10.0 - 1e-7, 1e-7, 0.0);
        try {
            MeshBoolean::cgal::plus(a, b);
            printf("H2b survived (no crash): result verts=%zu\n", a.its.vertices.size());
        } catch (const Slic3r::HardCrash &e) {
            printf("H2b HARDCRASH (guard caught GMP overflow in corefine): %s\n", e.what());
        } catch (const std::exception &e) {
            printf("H2b threw std::exception: %s\n", e.what());
        }
    });
}

// H2c: production corefine difference, dense coplanar grids, large coords.
TEST_CASE("H2c cgal::minus dense large-coord grids", "[MeshBoolean][!hide][repro]")
{
    run_on_worker_stack([] {
        TriangleMesh a = make_dense_box(20., 20., 20., 24);
        TriangleMesh b = make_dense_box(20., 20., 20., 24);
        scale(a, 1e6);
        scale(b, 1e6);
        translate(b, 20.0 * 1e6 - 1e-2, 1e-3, 1e-3);
        try {
            MeshBoolean::cgal::minus(a, b);
            printf("H2c survived (no crash): result verts=%zu\n", a.its.vertices.size());
        } catch (const Slic3r::HardCrash &e) {
            printf("H2c HARDCRASH (guard caught GMP overflow in corefine): %s\n", e.what());
        } catch (const std::exception &e) {
            printf("H2c threw std::exception: %s\n", e.what());
        }
    });
}
