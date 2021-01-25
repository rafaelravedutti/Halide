#include <algorithm>
#include <map>
#include <string>
#include <limits>

#include "PerfCtrProfiling.h"
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
using std::vector;

namespace Internal {

enum MarkersType {
    Marker_None,
    Marker_Prod,
    Marker_Cons,
    Marker_Overhead,
    Marker_Loop
};

struct InjectedMarkersData {
    int type;  // none, prod, cons, overhead or loop
    int id;    // id for function or loop
};

struct PerfCtrLoopData {
    string loop_name;   // loop name
    string var_name;    // variable name
    bool show_threads;  // show threads
};

vector<InjectedMarkersData> injected_markers;
vector<PerfCtrFuncData> perfctr_funcs_to_profile;
vector<PerfCtrLoopData> perfctr_loops_to_profile;

void internal_profile_at(LoopLevel loop_level, bool show_threads) {
    auto locked_loop_level = loop_level.lock();
    perfctr_loops_to_profile.push_back({locked_loop_level.func(), locked_loop_level.var().name(), show_threads});
}

void internal_profile_at(Func f, RVar var, bool show_threads) {
    auto locked_loop_level = LoopLevel(f, var).lock();
    perfctr_loops_to_profile.push_back({locked_loop_level.func(), locked_loop_level.var().name(), show_threads});
}

void internal_profile_at(Func f, Var var, bool show_threads) {
    auto locked_loop_level = LoopLevel(f, var).lock();
    perfctr_loops_to_profile.push_back({locked_loop_level.func(), locked_loop_level.var().name(), show_threads});
}

class InjectPerfCtrProfiling : public IRMutator {
public:
    map<string, int> indices;               // maps from func name -> index in buffer.
    map<string, int> loops;                 // maps from loop name -> index in buffer.
    map<int, int> show_threads_prod;        // maps from func int -> show threads in profiler in production level.
    map<int, int> show_threads_cons;        // maps from func int -> show threads in profiler in consumption level.
    map<int, int> show_threads_loop;        // maps from loop int -> show threads in profiler.
    map<int, uint64_t> func_childrens;      // map from func_id -> number of producer-consumer childrens
    vector<int> stack;                      // what produce nodes are we currently inside of.
    vector<pair<bool, bool>> profile_stack; // does the produce node we are inside of must be profiled?
    InjectPerfCtrProfiling() {
        indices["overhead"] = 0;
        stack.push_back(0);
    }

private:
    using IRMutator::visit;

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
        if(iter == indices.end()) {
            idx = (int)indices.size();
            indices[norm_name] = idx;
        } else {
            idx = iter->second;
        }
        return idx;
    }

    Stmt visit(const ProducerConsumer *op) override {
        int idx;
        Stmt body, stmt;
        std::string parent_name;
        bool must_profile = false;
        bool must_show_threads = false;
        bool inject_overhead_markers = false;
        int parent = (stack.size() > 1) ? stack.back() : 0;
        int func_id = get_func_id(op->name);
        int imd_id = (int) injected_markers.size();
        injected_markers.push_back({Marker_None, -1});

        for(auto& func: indices) {
            if(func.second == parent) {
                parent_name = func.first;
            }
        }

        auto it = find_if(perfctr_funcs_to_profile.begin(), perfctr_funcs_to_profile.end(),
                          [op, parent_name](PerfCtrFuncData const &elem) {
            bool level_cond = (elem.level & PROFILE_PRODUCTION && op->is_producer) || (elem.level & PROFILE_CONSUMPTION && !op->is_producer);
            bool parent_cond = (elem.parent_name == "") || (elem.parent_name == parent_name);
            return elem.func_name == op->name && level_cond && parent_cond;
        });

        must_profile = it != perfctr_funcs_to_profile.end() && it->enable;
        must_show_threads = must_profile && (it->level & PROFILE_SHOW_THREADS);

        if(stack.size() > 1) {
            func_childrens[stack.back()]++;
        }

        if (op->is_producer) {
            idx = func_id;
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

        if(!must_profile && it == perfctr_funcs_to_profile.end() && profile_stack.back().first) {
            must_profile = true;
            must_show_threads = profile_stack.back().second;
        }

        Expr profiler_token = Variable::make(Int(32), "profiler_token");
        Expr profiler_state = Variable::make(Handle(), "profiler_state");

        if(must_profile) {
            Expr enter_task, leave_task;

            if(op->is_producer && func_childrens[idx] > 0) {
                if(inject_overhead_markers) {
                    injected_markers[imd_id].type = Marker_Overhead;
                    injected_markers[imd_id].id = idx;
                    enter_task = Call::make(Int(32), "halide_perfctr_enter_overhead_region", {profiler_state, profiler_token, idx}, Call::Extern);
                    leave_task = Call::make(Int(32), "halide_perfctr_leave_overhead_region", {profiler_state, profiler_token, idx}, Call::Extern);
                    body = Block::make({Evaluate::make(enter_task), body, Evaluate::make(leave_task)});
                }
            } else {
                auto is_prod = make_bool(op->is_producer);
                injected_markers[imd_id].type = op->is_producer ? Marker_Prod : Marker_Cons;
                injected_markers[imd_id].id = func_id;
                enter_task = Call::make(Int(32), "halide_perfctr_enter_func", {profiler_state, profiler_token, func_id, is_prod}, Call::Extern);
                leave_task = Call::make(Int(32), "halide_perfctr_leave_func", {profiler_state, profiler_token, func_id, is_prod}, Call::Extern);
                body = Block::make({Evaluate::make(enter_task), body, Evaluate::make(leave_task)});
            }
        }

        stmt = ProducerConsumer::make(op->name, op->is_producer, body);

        // Since these are just markers to continue the parent's overhead region, they
        // do not need to be tracked for parallel regions later (or maybe they do?)
        if(inject_overhead_markers && must_profile && !profile_stack.empty()) {
            int parent = stack.back();
            Expr enter_overhead = Call::make(Int(32), "halide_perfctr_enter_overhead_region", {profiler_state, profiler_token, parent}, Call::Extern);
            Expr leave_overhead = Call::make(Int(32), "halide_perfctr_leave_overhead_region", {profiler_state, profiler_token, parent}, Call::Extern);
            stmt = Block::make({Evaluate::make(leave_overhead), stmt, Evaluate::make(enter_overhead)});
        }

        if(op->is_producer) {
          show_threads_prod[func_id] = must_show_threads;
        } else {
          show_threads_cons[func_id] = must_show_threads;
        }

        return stmt;
    }

    Stmt visit(const For *op) override {
        Stmt body = op->body;
        bool is_parallel_loop = (op->device_api == DeviceAPI::Hexagon || op->is_parallel());
        int imd_id = (int) injected_markers.size();
        injected_markers.push_back({Marker_None, -1});

        // We profile by storing a token to global memory, so don't enter GPU loops
        if(op->device_api == DeviceAPI::Hexagon) {
            // TODO: This is for all offload targets that support
            // limited internal profiling, which is currently just
            // hexagon. We don't support per-func stats remotely,
            // which means we can't do memory accounting.
            body = mutate(body);

            // Get the profiler state pointer from scratch inside the
            // kernel. There will be a separate copy of the state on
            // the DSP that the host side will periodically query.
            Expr get_state = Call::make(Handle(), "halide_perfctr_get_state", {}, Call::Extern);
            body = substitute("profiler_state", Variable::make(Handle(), "hvx_profiler_state"), body);
            body = LetStmt::make("hvx_profiler_state", get_state, body);
        } else if (op->device_api == DeviceAPI::None ||
                   op->device_api == DeviceAPI::Host) {
            body = mutate(body);
        } else {
            body = op->body;
        }

        Stmt stmt = For::make(op->name, op->min, op->extent, op->for_type, op->device_api, body);
        auto it = find_if(perfctr_loops_to_profile.begin(), perfctr_loops_to_profile.end(), [op](PerfCtrLoopData const &elem) {
            return starts_with(op->name, elem.loop_name + ".") && ends_with(op->name, "." + elem.var_name);
        });

        if(it != perfctr_loops_to_profile.end()) {
            Expr profiler_token = Variable::make(Int(32), "profiler_token");
            Expr profiler_state = Variable::make(Handle(), "profiler_state");
            int loop_id = (int) loops.size();
            injected_markers[imd_id].type = Marker_Loop;
            injected_markers[imd_id].id = loop_id;
            Expr enter_loop = Call::make(Int(32), "halide_perfctr_enter_loop", {profiler_state, profiler_token, loop_id}, Call::Extern);
            Expr leave_loop = Call::make(Int(32), "halide_perfctr_leave_loop", {profiler_state, profiler_token, loop_id}, Call::Extern);

            if(is_parallel_loop) {
                body = Block::make({Evaluate::make(enter_loop), body, Evaluate::make(leave_loop)});
                stmt = For::make(op->name, op->min, op->extent, op->for_type, op->device_api, body);
            } else {
                stmt = Block::make({Evaluate::make(enter_loop), stmt, Evaluate::make(leave_loop)});
            }

            loops[op->name] = loop_id;
            show_threads_loop[loop_id] = it->show_threads;
        }

        return stmt;
    }
};

class InjectPerfCtrParallelProfiling : public IRMutator {
public:
    InjectPerfCtrParallelProfiling() {
        last_imd = 0;
    }

private:
    vector<InjectedMarkersData> markers_stack;
    int last_imd;
    using IRMutator::visit;

    Stmt visit(const ProducerConsumer *op) override {
        Stmt body;
        int imd_id = last_imd++;

        if(injected_markers[imd_id].type != Marker_None) {
            markers_stack.push_back(injected_markers[imd_id]);
            body = mutate(op->body);
            markers_stack.pop_back();
        } else {
            body = mutate(op->body);
        }

        Stmt stmt = ProducerConsumer::make(op->name, op->is_producer, body);
        return stmt;
    }

    Stmt visit(const For *op) override {
        Stmt stmt, body;
        bool is_parallel_loop = (op->device_api == DeviceAPI::Hexagon || op->is_parallel());
        int imd_id = last_imd++;

        if(injected_markers[imd_id].type != Marker_None) {
            markers_stack.push_back(injected_markers[imd_id]);
            body = mutate(op->body);
            markers_stack.pop_back();
        } else {
            body = mutate(op->body);
        }

        if(is_parallel_loop && injected_markers[imd_id].type == Marker_None && markers_stack.size() > 0) {
            auto active_imd = markers_stack.back();
            auto id = active_imd.id;
            Expr profiler_token = Variable::make(Int(32), "profiler_token");
            Expr profiler_state = Variable::make(Handle(), "profiler_state");
            Expr enter_marker, leave_marker;

            switch(active_imd.type) {
                case Marker_Prod:
                    enter_marker = Call::make(Int(32), "halide_perfctr_enter_func", {profiler_state, profiler_token, id, make_bool(true)}, Call::Extern);
                    leave_marker = Call::make(Int(32), "halide_perfctr_leave_func", {profiler_state, profiler_token, id, make_bool(true)}, Call::Extern);
                    break;
                case Marker_Cons:
                    enter_marker = Call::make(Int(32), "halide_perfctr_enter_func", {profiler_state, profiler_token, id, make_bool(false)}, Call::Extern);
                    leave_marker = Call::make(Int(32), "halide_perfctr_leave_func", {profiler_state, profiler_token, id, make_bool(false)}, Call::Extern);
                    break;
                case Marker_Overhead:
                    enter_marker = Call::make(Int(32), "halide_perfctr_enter_overhead_region", {profiler_state, profiler_token, id}, Call::Extern);
                    leave_marker = Call::make(Int(32), "halide_perfctr_leave_overhead_region", {profiler_state, profiler_token, id}, Call::Extern);
                    break;
                case Marker_Loop:
                    enter_marker = Call::make(Int(32), "halide_perfctr_enter_loop", {profiler_state, profiler_token, id}, Call::Extern);
                    leave_marker = Call::make(Int(32), "halide_perfctr_leave_loop", {profiler_state, profiler_token, id}, Call::Extern);
                    break;
            }

            body = Block::make({Evaluate::make(enter_marker), body, Evaluate::make(leave_marker)});
            stmt = For::make(op->name, op->min, op->extent, op->for_type, op->device_api, body);
            stmt = Block::make({Evaluate::make(leave_marker), stmt, Evaluate::make(enter_marker)});
        } else {
            stmt = For::make(op->name, op->min, op->extent, op->for_type, op->device_api, body);
        }

        return stmt;
    }
};

Stmt inject_perfctr_profiling(Stmt s, string pipeline_name) {
    InjectPerfCtrProfiling profiling;
    s = profiling.mutate(s);
    InjectPerfCtrParallelProfiling parallel_profiling;
    s = parallel_profiling.mutate(s);

    int num_funcs = (int)(profiling.indices.size());
    int num_loops = (int)(profiling.loops.size());

    Expr func_names_buf = Variable::make(Handle(), "profiling_func_names");
    Expr loop_names_buf = Variable::make(Handle(), "profiling_loop_names");
    Expr func_threads_prod_buf = Variable::make(Handle(), "profiling_func_threads_prod");
    Expr func_threads_cons_buf = Variable::make(Handle(), "profiling_func_threads_cons");
    Expr loop_threads_buf = Variable::make(Handle(), "profiling_loop_threads");
    Expr start_profiler = Call::make(Int(32), "halide_perfctr_pipeline_start",
                                     {pipeline_name, num_funcs, num_loops, func_names_buf, loop_names_buf,
                                      func_threads_prod_buf, func_threads_cons_buf, loop_threads_buf}, Call::Extern);

    Expr get_state = Call::make(Handle(), "halide_perfctr_get_state", {}, Call::Extern);
    Expr get_pipeline_state = Call::make(Handle(), "halide_perfctr_get_pipeline_state", {pipeline_name}, Call::Extern);
    Expr profiler_token = Variable::make(Int(32), "profiler_token");
    Expr stop_profiler = Call::make(Handle(), Call::register_destructor, {Expr("halide_perfctr_pipeline_end"), get_state}, Call::Intrinsic);
    Expr profiler_state = Variable::make(Handle(), "profiler_state");

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
    s = Allocate::make("profiling_func_names", Handle(), MemoryType::Auto, {num_funcs}, const_true(), s);
    s = Allocate::make("profiling_loop_names", Handle(), MemoryType::Auto, {num_loops}, const_true(), s);
    s = Allocate::make("profiling_func_threads_prod", Handle(), MemoryType::Auto, {num_funcs}, const_true(), s);
    s = Allocate::make("profiling_func_threads_cons", Handle(), MemoryType::Auto, {num_funcs}, const_true(), s);
    s = Allocate::make("profiling_loop_threads", Handle(), MemoryType::Auto, {num_loops}, const_true(), s);
    s = Block::make(Evaluate::make(stop_profiler), s);
    return s;
}

}

void profile_at(LoopLevel loop_level, bool show_threads) { Internal::internal_profile_at(loop_level, show_threads); }
void profile_at(Func f, RVar var, bool show_threads) { Internal::internal_profile_at(f, var, show_threads); }
void profile_at(Func f, Var var, bool show_threads) { Internal::internal_profile_at(f, var, show_threads); }

}
