#ifndef SEARCH_SPACE_H
#define SEARCH_SPACE_H

#include "CostModel.h"
#include "DefaultCostModel.h"
#include "Featurization.h"
#include "FunctionDAG.h"
#include "LoopNest.h"
#include "PerfectHashMap.h"
#include "ASLog.h"
#include "State.h"
#include <set>
#include <unordered_set>
#include <vector>

namespace Halide {
namespace Internal {
namespace Autoscheduler {

struct SearchSpace {
    const FunctionDAG &dag;
    const MachineParams &params;
    const Target &target;
    CostModel *cost_model;
    Statistics &stats;

    NodeMap<bool> inlined_nodes;
    NodeMap<std::vector<IntrusivePtr<const LoopNest>>> compute_root_nodes;
    NodeMap<std::map<int, std::vector<IntrusivePtr<const LoopNest>>>> memoized_compute_root_blocks;

    SearchSpace(const FunctionDAG &dag,
                const MachineParams &params,
                const Target &target,
                CostModel *cost_model,
                Statistics &stats);

    // Sort / filter parallel tile options
    struct ParallelTileOption {
        vector<int64_t> outer_tiling;
        vector<int64_t> inner_tiling;
        double idle_core_wastage;
        bool entire;
        bool operator<(const ParallelTileOption &other) const {
            return idle_core_wastage < other.idle_core_wastage;
        }

        // Ensure we don't accidentally copy this type
        ParallelTileOption() = default;
        ParallelTileOption(ParallelTileOption &&) = default;
        ParallelTileOption &operator=(ParallelTileOption &&) = default;
        ParallelTileOption(const ParallelTileOption &) = delete;
        ParallelTileOption &operator=(const ParallelTileOption &) = delete;
    };

    vector<ParallelTileOption> filter_parallel_tile_options(IntrusivePtr<State> state,
                                                            const FunctionDAG::Node *node,
                                                            vector<vector<int64_t>>& inner_tilings,
                                                            const vector<int64_t>& pure_size) const;

    vector<ThreadTileOption> filter_thread_tile_options(vector<IntrusivePtr<const LoopNest>>& loop_nests) const;

    void memoize_blocks(const FunctionDAG::Node *node, LoopNest* new_root);

    bool add_states_from_memoized_blocks(IntrusivePtr<State> state,
                                         std::function<void(IntrusivePtr<State> &&)> &accept_child,
                                         const FunctionDAG::Node *node,
                                         int& num_children) const;


    // Generate successor states for given 'state'
    void generate_children(IntrusivePtr<State> state,
                           std::function<void(IntrusivePtr<State> &&)> &accept_child,
                           bool is_pre_pass);

    void freeze_lowest_cost_stages(const IntrusivePtr<State> best);
};



}  // namespace Autoscheduler
}  // namespace Internal
}  // namespace Halide

#endif // SEARCH_SPACE_H
