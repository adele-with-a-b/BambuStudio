#ifndef slic3r_CutSurfaceHelper_hpp_
#define slic3r_CutSurfaceHelper_hpp_

// CutSurfaceHelper -- run cut_surface() in a forked HELPER PROCESS so that
// CGAL/GMP stack-stomp crashes (the unrecoverable kind, where the recursion
// chain corrupts return addresses faster than siglongjmp can intercept)
// kill ONLY the helper, not the GUI. The parent reads the helper's exit
// status; on signal-death or timeout, returns an empty SurfaceCut just like
// the existing in-process recovery path does.
//
// Why subprocess instead of just bigger stack / signal handler:
//   The Apple crash report dated 2026-06-05 10:52:48 shows the failing
//   thread had only 2 real frames (`__gmpn_mul_1`, `__gmpn_matrix22_*`)
//   and a stomped PC. Kernel attribution: "Bad access in stack guard
//   region for thread 44 but crash was associated with thread 43 --
//   possible stray access?". The stack pointer walked off the end of
//   thread 43's 4 MB stack into the next thread's guard page. By the
//   time the kernel delivered SIGBUS, the return-address chain on
//   thread 43's stack had already been overwritten -- so siglongjmp
//   recovery from inside try_catch_signal landed in garbage and the
//   process died despite the existing tight-guard-around-corefine. A
//   bigger stack just delays this by a fraction of a second; the
//   underlying problem is GMP's unbounded-precision recursion on
//   numerically-near-degenerate input.
//
// Subprocess isolation makes recovery UNCONDITIONAL: whatever the helper
// does to its own address space, the parent sees only an exit code.

#include "CutSurface.hpp"  // SurfaceCut, ExPolygons, indexed_triangle_set
#include "Emboss.hpp"      // IProjection

namespace Slic3r {

// Drop-in replacement for cut_surface(): forks a helper process, runs the
// CGAL pipeline in there, returns the result via a pipe. On crash, hang,
// or any non-zero exit, returns an empty SurfaceCut so the caller can
// degrade gracefully (the existing toast-message path in EmbossJob.cpp).
//
// The helper is the same BambuStudio binary, re-exec'd with argv[1] ==
// "--cut-surface-helper". This avoids shipping/signing a second binary
// and inherits the parent's code signature + entitlements automatically.
//
// Deadline is wall-clock; helper that exceeds it gets SIGKILL.
SurfaceCut cut_surface_via_helper(const ExPolygons                        &shapes,
                                  const std::vector<indexed_triangle_set> &models,
                                  const Emboss::IProjection               &projection,
                                  float                                    projection_ratio,
                                  int                                      deadline_ms = 30000);

// Helper-process entry point. main() in BambuStudio.cpp dispatches here
// when argv[1] == "--cut-surface-helper". Reads serialized inputs from
// stdin, calls Slic3r::cut_surface(), serializes the result to stdout,
// returns 0 on clean completion. Returning non-zero (or dying by signal)
// is the parent's recovery cue.
int cut_surface_helper_main();

} // namespace Slic3r

#endif // slic3r_CutSurfaceHelper_hpp_
