#ifndef PERFORMCSGMESHBOOLEANS_HPP
#define PERFORMCSGMESHBOOLEANS_HPP

#include <stack>
#include <vector>

#include <boost/log/trivial.hpp>

#include "CSGMesh.hpp"

#include "libslic3r/Execution/ExecutionTBB.hpp"
//#include "libslic3r/Execution/ExecutionSeq.hpp"
#include "libslic3r/MeshBoolean.hpp"
#include "libslic3r/TryCatchSignal.hpp"
#include <csignal>

namespace Slic3r { namespace csg {
    enum class BooleanFailReason { OK, MeshEmpty, NotBoundAVolume, SelfIntersect, NoIntersection};

// This method can be overriden when a specific CSGPart type supports caching
// of the voxel grid
template<class CSGPartT>
MeshBoolean::cgal::CGALMeshPtr get_cgalmesh(const CSGPartT &csgpart)
{
    const indexed_triangle_set *its = csg::get_mesh(csgpart);
    indexed_triangle_set dummy;

    if (!its)
        its = &dummy;

    MeshBoolean::cgal::CGALMeshPtr ret;

    indexed_triangle_set m = *its;
    its_transform(m, get_transform(csgpart), true);

    // triangle_mesh_to_cgal calls CGAL Polygon_mesh_processing operations
    // (orient_polygon_soup, polygon_soup_to_polygon_mesh, orient_to_bound_a_volume)
    // which dispatch through CGAL Epeck (exact-rational) predicates internally,
    // regardless of the caller's surface-mesh kernel. On near-degenerate input
    // (e.g. an emboss text mesh with thin features + heavy boldness offset),
    // those exact-arithmetic paths can overflow the worker thread's stack via
    // unbounded GMP GCD recursion. The C++ try/catch below cannot catch a
    // SIGSEGV/SIGBUS/SIGFPE from that overflow -- the process dies with no
    // handler frame on the stack (observed: core.89638, core.31576). Wrap in
    // try_catch_signal so the overflow degrades to a null CGALMeshPtr, which
    // the existing return-null path already handles (the boolean check then
    // reports a normal "boolean not possible" rather than crashing the app).
    bool hw_fail = false;
    Slic3r::try_catch_signal({SIGSEGV, SIGBUS, SIGFPE},
        [&]() -> void {
            try {
                ret = MeshBoolean::cgal::triangle_mesh_to_cgal(m);
            } catch (...) {
                // errors are ignored, simply return null
                ret = nullptr;
            }
        },
        [&]{ hw_fail = true; });
    if (hw_fail) {
        BOOST_LOG_TRIVIAL(error) << "[EMBOSS-RECOVERY] csg::get_cgalmesh: CGAL Epeck stack overflow caught -- returning null mesh";
        ret = nullptr;
    }

    return ret;
}

// This method can be overriden when a specific CSGPart type supports caching
// of the voxel grid
template<class CSGPartT>
MeshBoolean::mcut::McutMeshPtr get_mcutmesh(const CSGPartT& csgpart)
{
    const indexed_triangle_set* its = csg::get_mesh(csgpart);
    indexed_triangle_set dummy;

    if (!its)
        its = &dummy;

    MeshBoolean::mcut::McutMeshPtr ret;

    indexed_triangle_set m = *its;
    its_transform(m, get_transform(csgpart), true);

    try {
        ret = MeshBoolean::mcut::triangle_mesh_to_mcut(m);
    }
    catch (...) {
        // errors are ignored, simply return null
        ret = nullptr;
    }

    return ret;
}

namespace detail_cgal {

using MeshBoolean::cgal::CGALMeshPtr;

inline void perform_csg(CSGType op, CGALMeshPtr &dst, CGALMeshPtr &src)
{
    if (!dst && op == CSGType::Union && src) {
        dst = std::move(src);
        return;
    }

    if (!dst || !src)
        return;

    switch (op) {
    case CSGType::Union:
        MeshBoolean::cgal::plus(*dst, *src);
        break;
    case CSGType::Difference:
        MeshBoolean::cgal::minus(*dst, *src);
        break;
    case CSGType::Intersection:
        MeshBoolean::cgal::intersect(*dst, *src);
        break;
    }
}

template<class Ex, class It>
std::vector<CGALMeshPtr> get_cgalptrs(Ex policy, const Range<It> &csgrange)
{
    std::vector<CGALMeshPtr> ret(csgrange.size());
    execution::for_each(policy, size_t(0), csgrange.size(),
                        [&csgrange, &ret](size_t i) {
        auto it = csgrange.begin();
        std::advance(it, i);
        auto &csgpart = *it;
        ret[i]        = get_cgalmesh(csgpart);
    });

    return ret;
}

} // namespace detail

namespace detail_mcut {

    using MeshBoolean::mcut::McutMeshPtr;

    inline void perform_csg(CSGType op, McutMeshPtr& dst, McutMeshPtr& src)
    {
        if (!dst && op == CSGType::Union && src) {
            dst = std::move(src);
            return;
        }

        if (!dst || !src)
            return;

        switch (op) {
        case CSGType::Union:
            MeshBoolean::mcut::do_boolean(*dst, *src,"UNION");
            break;
        case CSGType::Difference:
            MeshBoolean::mcut::do_boolean(*dst, *src,"A_NOT_B");
            break;
        case CSGType::Intersection:
            MeshBoolean::mcut::do_boolean(*dst, *src,"INTERSECTION");
            break;
        }
    }

    template<class Ex, class It>
    std::vector<McutMeshPtr> get_mcutptrs(Ex policy, const Range<It>& csgrange)
    {
        std::vector<McutMeshPtr> ret(csgrange.size());
        execution::for_each(policy, size_t(0), csgrange.size(),
            [&csgrange, &ret](size_t i) {
                auto it = csgrange.begin();
                std::advance(it, i);
                auto& csgpart = *it;
                ret[i] = get_mcutmesh(csgpart);
            });

        return ret;
    }

} // namespace mcut_detail

// Process the sequence of CSG parts with CGAL.
template<class It>
void perform_csgmesh_booleans_cgal(MeshBoolean::cgal::CGALMeshPtr &cgalm,
                              const Range<It>                &csgrange)
{
    using MeshBoolean::cgal::CGALMesh;
    using MeshBoolean::cgal::CGALMeshPtr;
    using namespace detail_cgal;

    struct Frame {
        CSGType op; CGALMeshPtr cgalptr;
        explicit Frame(CSGType csgop = CSGType::Union)
            : op{ csgop }
            , cgalptr{ MeshBoolean::cgal::triangle_mesh_to_cgal(indexed_triangle_set{}) }
        {}
    };

    std::stack opstack{ std::vector<Frame>{} };

    opstack.push(Frame{});

    std::vector<CGALMeshPtr> cgalmeshes = get_cgalptrs(ex_tbb, csgrange);

    size_t csgidx = 0;
    for (auto& csgpart : csgrange) {

        auto op = get_operation(csgpart);
        CGALMeshPtr& cgalptr = cgalmeshes[csgidx++];

        if (get_stack_operation(csgpart) == CSGStackOp::Push) {
            opstack.push(Frame{ op });
            op = CSGType::Union;
        }

        Frame* top = &opstack.top();

        perform_csg(get_operation(csgpart), top->cgalptr, cgalptr);

        if (get_stack_operation(csgpart) == CSGStackOp::Pop) {
            CGALMeshPtr src = std::move(top->cgalptr);
            auto popop = opstack.top().op;
            opstack.pop();
            CGALMeshPtr& dst = opstack.top().cgalptr;
            perform_csg(popop, dst, src);
        }
    }

    cgalm = std::move(opstack.top().cgalptr);
}

// Process the sequence of CSG parts with mcut.
template<class It>
void perform_csgmesh_booleans_mcut(MeshBoolean::mcut::McutMeshPtr& mcutm,
    const Range<It>& csgrange)
{
    using MeshBoolean::mcut::McutMesh;
    using MeshBoolean::mcut::McutMeshPtr;
    using namespace detail_mcut;

    struct Frame {
        CSGType op; McutMeshPtr mcutptr;
        explicit Frame(CSGType csgop = CSGType::Union)
            : op{ csgop }
            , mcutptr{ MeshBoolean::mcut::triangle_mesh_to_mcut(indexed_triangle_set{}) }
        {}
    };

    std::stack opstack{ std::vector<Frame>{} };

    opstack.push(Frame{});

    std::vector<McutMeshPtr> McutMeshes = get_mcutptrs(ex_tbb, csgrange);

    size_t csgidx = 0;
    for (auto& csgpart : csgrange) {

        auto op = get_operation(csgpart);
        McutMeshPtr& mcutptr = McutMeshes[csgidx++];

        if (get_stack_operation(csgpart) == CSGStackOp::Push) {
            opstack.push(Frame{ op });
            op = CSGType::Union;
        }

        Frame* top = &opstack.top();

        perform_csg(get_operation(csgpart), top->mcutptr, mcutptr);

        if (get_stack_operation(csgpart) == CSGStackOp::Pop) {
            McutMeshPtr src = std::move(top->mcutptr);
            auto popop = opstack.top().op;
            opstack.pop();
            McutMeshPtr& dst = opstack.top().mcutptr;
            perform_csg(popop, dst, src);
        }
    }

    mcutm = std::move(opstack.top().mcutptr);

}


template<class It, class Visitor>
std::tuple<BooleanFailReason,std::string> check_csgmesh_booleans(const Range<It> &csgrange, Visitor &&vfn)
{
    using namespace detail_cgal;
    BooleanFailReason fail_reason = BooleanFailReason::OK;
    std::string fail_part_name;
    std::vector<CGALMeshPtr> cgalmeshes(csgrange.size());
    auto check_part = [&csgrange, &cgalmeshes,&fail_reason,&fail_part_name](size_t i)
    {
        auto it = csgrange.begin();
        std::advance(it, i);
        auto &csgpart = *it;
        auto m = get_cgalmesh(csgpart);

        // mesh can be nullptr if this is a stack push or pull
        if (!get_mesh(csgpart) && get_stack_operation(csgpart) != CSGStackOp::Continue) {
            cgalmeshes[i] = MeshBoolean::cgal::triangle_mesh_to_cgal(indexed_triangle_set{});
            return;
        }

        try {
            if (!m || MeshBoolean::cgal::empty(*m)) {
                BOOST_LOG_TRIVIAL(info) << "check_csgmesh_booleans fails! mesh " << i << "/" << csgrange.size() << " is empty, cannot do boolean!";
                fail_reason= BooleanFailReason::MeshEmpty;
                fail_part_name = csgpart.name;
                return;
            }

            /*if (!MeshBoolean::cgal::does_bound_a_volume(*m)) {//has crash problem
                BOOST_LOG_TRIVIAL(info) << "check_csgmesh_booleans fails! mesh "<<i<<"/"<<csgrange.size()<<" does_bound_a_volume is false, cannot do boolean!";
                fail_reason= BooleanFailReason::NotBoundAVolume;
                fail_part_name = csgpart.name;
                return;
            }*/

            // does_self_intersect uses CGAL Polygon_mesh_processing which
            // dispatches through Epeck exact-rational arithmetic internally.
            // On near-degenerate meshes (e.g. emboss text with thin features
            // and heavy boldness offset), the exact-arithmetic recursion can
            // overflow the worker stack via unbounded GMP GCD. The C++ try/catch
            // below cannot catch SIGSEGV/SIGBUS/SIGFPE -- the process dies with
            // no handler frame. Wrap in try_catch_signal so an overflow degrades
            // to "treat as self-intersecting" (the existing failure path),
            // letting the slicer surface a graceful error instead of crashing.
            // (cf. the commented-out does_bound_a_volume above marked "has crash
            // problem" -- same root cause; this one wasn't disabled.)
            bool si_overflow = false;
            bool si_result = false;
            Slic3r::try_catch_signal({SIGSEGV, SIGBUS, SIGFPE},
                [&]() -> void { si_result = MeshBoolean::cgal::does_self_intersect(*m); },
                [&]{ si_overflow = true; });
            if (si_overflow) {
                BOOST_LOG_TRIVIAL(error) << "check_csgmesh_booleans: does_self_intersect stack-overflowed on mesh " << i << "/" << csgrange.size() << "; treating as self-intersecting";
                fail_reason = BooleanFailReason::SelfIntersect;
                fail_part_name = csgpart.name;
                return;
            }
            if (si_result) {
                BOOST_LOG_TRIVIAL(info) << "check_csgmesh_booleans fails! mesh " << i << "/" << csgrange.size() << " does_self_intersect is true, cannot do boolean!";
                fail_reason= BooleanFailReason::SelfIntersect;
                fail_part_name = csgpart.name;
                return;
            }
        }
        catch (...) { return; }

        cgalmeshes[i] = std::move(m);
    };
    execution::for_each(ex_tbb, size_t(0), csgrange.size(), check_part);

    //It ret = csgrange.end();
    //for (size_t i = 0; i < csgrange.size(); ++i) {
    //    if (!cgalmeshes[i]) {
    //        auto it = csgrange.begin();
    //        std::advance(it, i);
    //        vfn(it);

    //        if (ret == csgrange.end())
    //            ret = it;
    //    }
    //}

    return { fail_reason,fail_part_name };
}

template<class It>
std::tuple<BooleanFailReason, std::string> check_csgmesh_booleans(const Range<It> &csgrange, bool use_mcut=false)
{
    if(!use_mcut)
        return check_csgmesh_booleans(csgrange, [](auto &) {});
    else {
        using namespace detail_mcut;
        BooleanFailReason fail_reason = BooleanFailReason::OK;
        std::string fail_part_name;

        std::vector<McutMeshPtr> McutMeshes(csgrange.size());
        auto check_part = [&csgrange, &McutMeshes,&fail_reason,&fail_part_name](size_t i) {
            auto it = csgrange.begin();
            std::advance(it, i);
            auto& csgpart = *it;
            auto m = get_mcutmesh(csgpart);

            // mesh can be nullptr if this is a stack push or pull
            if (!get_mesh(csgpart) && get_stack_operation(csgpart) != CSGStackOp::Continue) {
                McutMeshes[i] = MeshBoolean::mcut::triangle_mesh_to_mcut(indexed_triangle_set{});
                return;
            }

            try {
                if (!m || MeshBoolean::mcut::empty(*m)) {
                    fail_reason=BooleanFailReason::MeshEmpty;
                    fail_part_name = csgpart.name;
                    return;
                }
            }
            catch (...) { return; }

            McutMeshes[i] = std::move(m);
        };
        execution::for_each(ex_tbb, size_t(0), csgrange.size(), check_part);
        return { fail_reason,fail_part_name };
    }
}

template<class It>
MeshBoolean::cgal::CGALMeshPtr perform_csgmesh_booleans(const Range<It> &csgparts)
{
    auto ret = MeshBoolean::cgal::triangle_mesh_to_cgal(indexed_triangle_set{});
    if (ret)
        perform_csgmesh_booleans_cgal(ret, csgparts);
    return ret;
}

template<class It>
MeshBoolean::mcut::McutMeshPtr  perform_csgmesh_booleans_mcut(const Range<It>& csgparts)
{
    auto ret = MeshBoolean::mcut::triangle_mesh_to_mcut(indexed_triangle_set{});
    if (ret)
        perform_csgmesh_booleans_mcut(ret, csgparts);
    return ret;
}

} // namespace csg
} // namespace Slic3r

#endif // PERFORMCSGMESHBOOLEANS_HPP
