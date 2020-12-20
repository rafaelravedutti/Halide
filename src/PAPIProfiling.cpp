#include <algorithm>
#include <map>
#include <string>
#include <limits>

#include "PAPIProfiling.h"
#include "CodeGen_Internal.h"
#include "IRMutator.h"
#include "IROperator.h"
#include "Scope.h"
#include "Simplify.h"
#include "Substitute.h"
#include "Util.h"
#include "Func.h"

namespace Halide {

using std::map;
using std::pair;
using std::string;
using std::tuple;
using std::vector;

vector<tuple<string, string, bool>> papi_profiler_loops;

void profile_at(LoopLevel loop_level, bool show_threads) {
    auto locked_loop_level = loop_level.lock();
    papi_profiler_loops.push_back(std::make_tuple(locked_loop_level.func(), locked_loop_level.var().name(), show_threads));
}

void profile_at(Func f, RVar var, bool show_threads) {
    auto locked_loop_level = LoopLevel(f, var).lock();
    papi_profiler_loops.push_back(std::make_tuple(locked_loop_level.func(), locked_loop_level.var().name(), show_threads));
}

void profile_at(Func f, Var var, bool show_threads) {
    auto locked_loop_level = LoopLevel(f, var).lock();
    papi_profiler_loops.push_back(std::make_tuple(locked_loop_level.func(), locked_loop_level.var().name(), show_threads));
}

namespace Internal {

vector<tuple<string, int, bool, string>> papi_profiler_defs;

class InjectPAPIProfiling : public IRMutator {
public:
    map<string, int> indices;   // maps from func name -> index in buffer.
    map<string, int> loops;   // maps from loop name -> index in buffer.
    map<int, int> show_threads_prod;   // maps from func int -> show threads in profiler in production level.
    map<int, int> show_threads_cons;   // maps from func int -> show threads in profiler in consumption level.
    map<int, int> show_threads_loop;   // maps from loop int -> show threads in profiler.

    vector<int> stack; // What produce nodes are we currently inside of.
    vector<pair<bool, bool>> profile_stack; // Produce node we are inside of must be profiled?

    string pipeline_name;

    InjectPAPIProfiling(const string &pipeline_name) : pipeline_name(pipeline_name) {
        indices["overhead"] = 0;
        stack.push_back(0);
    }

    map<int, uint64_t> func_childrens; // map from func_id -> number of producer-consumer childrens
    map<int, uint64_t> func_stack_current; // map from func id -> current stack allocation
    map<int, uint64_t> func_stack_peak; // map from func id -> peak stack allocation

private:
    using IRMutator::visit;

    struct AllocSize {
        bool on_stack;
        Expr size;
    };

    Scope<AllocSize> func_alloc_sizes;

    bool profiling_memory = true;

    // Strip down the tuple name, e.g. f.0 into f
    string normalize_name(const string &name) {
        vector<string> v = split_string(name, ".");
        internal_assert(v.size() > 0);
        return v[0];
    }

    int get_func_id(const string &name) {
        string norm_name = normalize_name(name);
        int idx = -1;
        map<string, int>::iterator iter = indices.find(norm_name);
        if (iter == indices.end()) {
            idx = (int)indices.size();
            indices[norm_name] = idx;
        } else {
            idx = iter->second;
        }
        return idx;
    }

    Expr compute_allocation_size(const vector<Expr> &extents,
                                 const Expr &condition,
                                 const Type &type,
                                 const std::string &name,
                                 bool &on_stack) {
        on_stack = true;

        Expr cond = simplify(condition);
        if (is_const_zero(cond)) { // Condition always false
            return make_zero(UInt(64));
        }

        int32_t constant_size = Allocate::constant_allocation_size(extents, name);
        if (constant_size > 0) {
            int64_t stack_bytes = constant_size * type.bytes();
            if (can_allocation_fit_on_stack(stack_bytes)) { // Allocation on stack
                return make_const(UInt(64), stack_bytes);
            }
        }

        // Check that the allocation is not scalar (if it were scalar
        // it would have constant size).
        internal_assert(extents.size() > 0);

        on_stack = false;
        Expr size = cast<uint64_t>(extents[0]);
        for (size_t i = 1; i < extents.size(); i++) {
            size *= extents[i];
        }
        size = simplify(Select::make(condition, size * type.bytes(), make_zero(UInt(64))));
        return size;
    }

    Stmt visit(const ProducerConsumer *op) override {
        int idx;
        Stmt body, stmt;
        bool must_profile = false;
        bool must_show_threads = false;

        int parent = (stack.size() > 1) ? stack.back() : 0;
        std::string parent_name;

        for(auto& func: indices) {
          if(func.second == parent) {
            parent_name = func.first;
          }
        }

        auto it = find_if(papi_profiler_defs.begin(), papi_profiler_defs.end(),
                          [op, parent_name](std::tuple<string, int, bool, string> const &elem) {
            bool level_cond =
                (std::get<1>(elem) & PROFILE_PRODUCTION && op->is_producer) ||
                (std::get<1>(elem) & PROFILE_CONSUMPTION && !op->is_producer);

            bool parent_cond =
                (std::get<3>(elem) == "") || (std::get<3>(elem) == parent_name);

            return std::get<0>(elem) == op->name && level_cond && parent_cond;
        });

        must_profile = it != papi_profiler_defs.end() && std::get<2>(*it);
        must_show_threads = must_profile && std::get<1>(*it) & PROFILE_SHOW_THREADS;

        if(stack.size() > 1) {
            func_childrens[stack.back()]++;
        }

        if (op->is_producer) {
            idx = get_func_id(op->name);
            func_childrens[idx] = 0;

            profile_stack.push_back(std::make_pair(must_profile, must_show_threads));
            stack.push_back(idx);
            body = mutate(op->body);
            stack.pop_back();
            profile_stack.pop_back();
        } else {
            body = mutate(op->body);
            // At the beginning of the consume step, set the current task
            // back to the outer one.
            idx = stack.back();
        }

        if(!must_profile && it == papi_profiler_defs.end() && profile_stack.back().first) {
          must_profile = true;
          must_show_threads = profile_stack.back().second;
        }

        Expr profiler_token = Variable::make(Int(32), "profiler_token");
        Expr profiler_state = Variable::make(Handle(), "profiler_state");

        if(must_profile) {
            Expr enter_task, leave_task;

            if(op->is_producer && func_childrens[idx] > 0) {
                enter_task = Call::make(Int(32), "halide_papi_enter_overhead_region",
                                        {profiler_state, profiler_token, idx}, Call::Extern);

                leave_task = Call::make(Int(32), "halide_papi_leave_overhead_region",
                                        {profiler_state, profiler_token, idx}, Call::Extern);
            } else {
                enter_task = Call::make(Int(32), "halide_papi_enter_current_func",
                                        {profiler_state, profiler_token, get_func_id(op->name),
                                         make_bool(op->is_producer)}, Call::Extern);

                leave_task = Call::make(Int(32), "halide_papi_leave_current_func",
                                        {profiler_state, profiler_token, get_func_id(op->name),
                                         make_bool(op->is_producer)}, Call::Extern);
            }

            body = Block::make({Evaluate::make(enter_task), body, Evaluate::make(leave_task)});
        }

        // This call gets inlined and becomes a single store instruction.
        Expr set_task = Call::make(Int(32), "halide_papi_set_current_func",
                                   {profiler_state, profiler_token, idx}, Call::Extern);

        body = Block::make(Evaluate::make(set_task), body);

        stmt = ProducerConsumer::make(op->name, op->is_producer, body);

        if(must_profile && !profile_stack.empty()) {
            int parent = stack.back();

            Expr enter_overhead = Call::make(Int(32), "halide_papi_enter_overhead_region",
                                             {profiler_state, profiler_token, parent}, Call::Extern);

            Expr leave_overhead = Call::make(Int(32), "halide_papi_leave_overhead_region",
                                             {profiler_state, profiler_token, parent}, Call::Extern);

            stmt = Block::make({Evaluate::make(leave_overhead), stmt, Evaluate::make(enter_overhead)});
        }

        auto fid = get_func_id(op->name);

        if(op->is_producer) {
          show_threads_prod[fid] = must_show_threads;
        } else {
          show_threads_cons[fid] = must_show_threads;
        }

        return stmt;
    }

    Stmt visit(const For *op) override {

        Stmt body = op->body;

        // The for loop indicates a device transition or a
        // parallel job launch. Decrement the number of active
        // threads outside the loop, and increment it inside the
        // body.
        bool update_active_threads = (op->device_api == DeviceAPI::Hexagon ||
                                      op->is_parallel());

        Expr state = Variable::make(Handle(), "profiler_state");

        Stmt enter_parallel_region =
            Evaluate::make(Call::make(Int(32), "halide_papi_enter_parallel_region",
                                      {state}, Call::Extern));
        Stmt leave_parallel_region =
            Evaluate::make(Call::make(Int(32), "halide_papi_leave_parallel_region",
                                      {state}, Call::Extern));

        Stmt incr_active_threads =
            Evaluate::make(Call::make(Int(32), "halide_papi_incr_active_threads",
                                      {state}, Call::Extern));
        Stmt decr_active_threads =
            Evaluate::make(Call::make(Int(32), "halide_papi_decr_active_threads",
                                      {state}, Call::Extern));

        if (update_active_threads) {
            body = Block::make({incr_active_threads, body, decr_active_threads});
        }

        // We profile by storing a token to global memory, so don't enter GPU loops
        if (op->device_api == DeviceAPI::Hexagon) {
            // TODO: This is for all offload targets that support
            // limited internal profiling, which is currently just
            // hexagon. We don't support per-func stats remotely,
            // which means we can't do memory accounting.
            bool old_profiling_memory = profiling_memory;
            profiling_memory = false;
            body = mutate(body);

            profiling_memory = old_profiling_memory;

            // Get the profiler state pointer from scratch inside the
            // kernel. There will be a separate copy of the state on
            // the DSP that the host side will periodically query.
            Expr get_state = Call::make(Handle(), "halide_papi_get_state", {}, Call::Extern);
            body = substitute("profiler_state", Variable::make(Handle(), "hvx_profiler_state"), body);
            body = LetStmt::make("hvx_profiler_state", get_state, body);
        } else if (op->device_api == DeviceAPI::None ||
                   op->device_api == DeviceAPI::Host) {
            body = mutate(body);
        } else {
            body = op->body;
        }

        Stmt stmt = For::make(op->name, op->min, op->extent, op->for_type, op->device_api, body);

        auto it = find_if(papi_profiler_loops.begin(), papi_profiler_loops.end(),
                          [op](std::tuple<string, string, bool> const &elem) {
            return starts_with(op->name, std::get<0>(elem) + ".") && ends_with(op->name, "." + std::get<1>(elem));
        });

        if (it != papi_profiler_loops.end()) {
            Expr profiler_token = Variable::make(Int(32), "profiler_token");
            Expr profiler_state = Variable::make(Handle(), "profiler_state");
            int loop_id = (int) loops.size();

            Stmt enter_loop = Evaluate::make(Call::make(Int(32), "halide_papi_enter_loop",
                                             {profiler_state, profiler_token, make_const(UInt(64), loop_id)}, Call::Extern));

            Stmt leave_loop = Evaluate::make(Call::make(Int(32), "halide_papi_leave_loop",
                                             {profiler_state, profiler_token, make_const(UInt(64), loop_id)}, Call::Extern));

            stmt = Block::make({enter_loop, stmt, leave_loop});

            loops[op->name] = loop_id;
            show_threads_loop[loop_id] = std::get<2>(*it);
        }

        if (update_active_threads) {
            stmt = Block::make({decr_active_threads, enter_parallel_region, stmt, leave_parallel_region, incr_active_threads});
        }
        return stmt;
    }
};

Stmt inject_papi_profiling(Stmt s, string pipeline_name) {
    InjectPAPIProfiling profiling(pipeline_name);
    s = profiling.mutate(s);

    int num_funcs = (int)(profiling.indices.size());
    int num_loops = (int)(profiling.loops.size());

    Expr func_names_buf = Variable::make(Handle(), "profiling_func_names");
    Expr loop_names_buf = Variable::make(Handle(), "profiling_loop_names");

    Expr func_threads_prod_buf = Variable::make(Handle(), "profiling_func_threads_prod");
    Expr func_threads_cons_buf = Variable::make(Handle(), "profiling_func_threads_cons");
    Expr loop_threads_buf = Variable::make(Handle(), "profiling_loop_threads");

    Expr start_profiler = Call::make(Int(32), "halide_papi_pipeline_start",
                                     {pipeline_name, num_funcs, num_loops, func_names_buf, loop_names_buf,
                                      func_threads_prod_buf, func_threads_cons_buf, loop_threads_buf}, Call::Extern);

    Expr get_state = Call::make(Handle(), "halide_papi_get_state", {}, Call::Extern);

    Expr get_pipeline_state = Call::make(Handle(), "halide_papi_get_pipeline_state", {pipeline_name}, Call::Extern);

    Expr profiler_token = Variable::make(Int(32), "profiler_token");

    //Expr stop_profiler = Call::make(Int(32), Call::register_destructor,
    //                                {Expr("halide_papi_pipeline_end"), get_state}, Call::Intrinsic);
    Expr stop_profiler = Call::make(Handle(), Call::register_destructor,
                                    {Expr("halide_papi_pipeline_end"), get_state}, Call::Intrinsic);

    Expr profiler_state = Variable::make(Handle(), "profiler_state");

    Stmt incr_active_threads =
        Evaluate::make(Call::make(Int(32), "halide_papi_incr_active_threads",
                                  {profiler_state}, Call::Extern));
    Stmt decr_active_threads =
        Evaluate::make(Call::make(Int(32), "halide_papi_decr_active_threads",
                                  {profiler_state}, Call::Extern));
    s = Block::make({incr_active_threads, s, decr_active_threads});

    s = LetStmt::make("profiler_pipeline_state", get_pipeline_state, s);
    s = LetStmt::make("profiler_state", get_state, s);

    // If there was a problem starting the profiler, it will call an
    // appropriate halide error function and then return the
    // (negative) error code as the token.
    s = Block::make(AssertStmt::make(profiler_token >= 0, profiler_token), s);
    s = LetStmt::make("profiler_token", start_profiler, s);

    for (std::pair<string, int> p : profiling.indices) {
        s = Block::make(Store::make("profiling_func_names", p.first, p.second, Parameter(), const_true(), ModulusRemainder()), s);
    }

    for (std::pair<string, int> p : profiling.loops) {
        s = Block::make(Store::make("profiling_loop_names", p.first, p.second, Parameter(), const_true(), ModulusRemainder()), s);
    }

    for (std::pair<int, int> p : profiling.show_threads_prod) {
        s = Block::make(Store::make("profiling_func_threads_prod", p.second, p.first, Parameter(), const_true(), ModulusRemainder()), s);
    }

    for (std::pair<int, int> p : profiling.show_threads_cons) {
        s = Block::make(Store::make("profiling_func_threads_cons", p.second, p.first, Parameter(), const_true(), ModulusRemainder()), s);
    }

    for (std::pair<int, int> p : profiling.show_threads_loop) {
        s = Block::make(Store::make("profiling_loop_threads", p.second, p.first, Parameter(), const_true(), ModulusRemainder()), s);
    }

    s = Block::make(s, Free::make("profiling_func_threads_prod"));
    s = Block::make(s, Free::make("profiling_func_threads_cons"));
    s = Block::make(s, Free::make("profiling_loop_threads"));
    s = Block::make(s, Free::make("profiling_func_names"));
    s = Block::make(s, Free::make("profiling_loop_names"));

    s = Allocate::make("profiling_func_names", Handle(),
                       MemoryType::Auto, {num_funcs}, const_true(), s);
    s = Allocate::make("profiling_loop_names", Handle(),
                       MemoryType::Auto, {num_loops}, const_true(), s);
    s = Allocate::make("profiling_func_threads_prod", Handle(),
                       MemoryType::Auto, {num_funcs}, const_true(), s);
    s = Allocate::make("profiling_func_threads_cons", Handle(),
                       MemoryType::Auto, {num_funcs}, const_true(), s);
    s = Allocate::make("profiling_loop_threads", Handle(),
                       MemoryType::Auto, {num_loops}, const_true(), s);

    s = Block::make(Evaluate::make(stop_profiler), s);

    return s;
}

}
}
