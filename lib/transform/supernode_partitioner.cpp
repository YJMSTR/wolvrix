#include "transform/supernode_partitioner.hpp"
#include <algorithm>
#include <limits>
#include <stdexcept>

namespace wolvrix::lib::transform
{

SuperNodePartitioner::SuperNodePartitioner(SuperNodeGraph& sg)
    : sg_(sg) {}

void SuperNodePartitioner::partition() {
    // Validate input: check for cycles
    if (sg_.hasCircularDependency()) {
        throw std::runtime_error("Cannot partition graph with circular dependencies");
    }

    // Validate input: check for empty graph
    if (sg_.nodeCount() == 0) {
        return;
    }

    // Cache topological order for efficiency
    cachedTopoOrder_ = sg_.topologicalSort();

    auto cuts = computeOptimalCuts();

    // Validate cut sequence is strictly increasing
    for (size_t i = 1; i < cuts.size(); ++i) {
        if (cuts[i] <= cuts[i-1]) {
            // Invalid cut sequence, abort
            return;
        }
    }

    mergeByIntervals(cuts);

    // Recompute metadata after interval merging
    cachedTopoOrder_ = sg_.topologicalSort();
}

std::vector<int> SuperNodePartitioner::computeOptimalCuts() {
    // Use cached topological order
    const auto& sorted = cachedTopoOrder_;
    int n = sorted.size();

    std::vector<DPState> dp(n + 1);
    dp[0].cost = 0;
    dp[0].backtrack = -1;

    for (int i = 1; i <= n; i++) {
        dp[i].cost = std::numeric_limits<int>::max();

        // Try all valid starting positions
        for (int j = 0; j < i; j++) {
            // Check if interval [j, i) respects size constraint
            size_t totalMembers = 0;
            for (int k = j; k < i; k++) {
                totalMembers += sg_.getNode(sorted[k]).memberCount();
            }

            // Skip if interval exceeds max size
            if (totalMembers > maxSuperNodeSize_) {
                continue;
            }

            int cutCost = computeCutCost(j, i);
            int totalCost = dp[j].cost + cutCost;

            if (totalCost < dp[i].cost) {
                dp[i].cost = totalCost;
                dp[i].backtrack = j;
                dp[i].cutCost = cutCost;
            }
        }
    }

    std::vector<int> cuts;
    int pos = n;
    while (pos > 0) {
        cuts.push_back(pos);
        pos = dp[pos].backtrack;
    }
    std::reverse(cuts.begin(), cuts.end());
    return cuts;
}

int SuperNodePartitioner::computeCutCost(int start, int end) const {
    // Use cached topological order
    const auto& sorted = cachedTopoOrder_;
    int cost = 0;

    for (int i = start; i < end; i++) {
        for (auto succId : sg_.successors(sorted[i])) {
            bool isExternal = true;
            for (int j = start; j < end; j++) {
                if (sorted[j] == succId) {
                    isExternal = false;
                    break;
                }
            }
            if (isExternal) {
                cost++;
            }
        }
    }

    return cost;
}

void SuperNodePartitioner::mergeByIntervals(const std::vector<int>& cuts) {
    // Use cached topological order
    const auto& sorted = cachedTopoOrder_;
    int start = 0;

    for (int cut : cuts) {
        if (cut > start + 1) {
            SuperNodeId targetId = sorted[start];
            for (int i = start + 1; i < cut; i++) {
                sg_.merge(targetId, sorted[i]);
            }
        }
        start = cut;
    }
}

} // namespace wolvrix::lib::transform
