#include "transform/supernode_partitioner.hpp"
#include <algorithm>
#include <limits>

namespace wolvrix::lib::transform
{

SuperNodePartitioner::SuperNodePartitioner(SuperNodeGraph& sg)
    : sg_(sg) {}

void SuperNodePartitioner::partition() {
    auto cuts = computeOptimalCuts();
    mergeByIntervals(cuts);
}

std::vector<int> SuperNodePartitioner::computeOptimalCuts() {
    auto sorted = sg_.topologicalSort();
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
    auto sorted = sg_.topologicalSort();
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
    auto sorted = sg_.topologicalSort();
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
