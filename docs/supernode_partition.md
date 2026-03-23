# SuperNode Partition Pass

## Overview

The SuperNode Partition Pass implements GSim's hypergraph partitioning algorithm for Wolvrix. It analyzes timing domains, coarsens operations into supernodes, and partitions them to optimize scheduling and reduce cross-domain communication.

## Usage

```bash
wolvrix-transform --pass=supernode-partition input.json -o output.json
```

The pass writes metadata to scratchpad for downstream passes:

- `supernode.count`: Total number of supernodes
- `supernode.avg_size`: Average supernode size (operation count)
- `supernode.max_size`: Maximum supernode size
- `supernode.edge_count`: Total edges between supernodes
- `supernode.cut_edges`: Edges crossing partition boundaries
- `supernode.cross_domain_edges`: Edges crossing timing domains

## Algorithm

### 1. Timing Domain Analysis
Analyzes event keys to identify timing domains and detect cross-domain edges.

### 2. Coarsening
Applies 5 merge strategies in order:
- **mergeResetAll**: Groups nodes by reset signal
- **mergeWhenNodes**: Groups nodes by conditional signals
- **mergeOut1**: Merges nodes with single successor
- **mergeIn1**: Merges nodes with single predecessor
- **mergeSublings**: Merges nodes with identical predecessor patterns

### 3. Partitioning
Uses dynamic programming to find optimal cuts that minimize cross-partition edges while respecting size constraints.

## Constraints

- Supernodes respect timing domain boundaries (no cross-domain merges)
- Maximum supernode size: 35 operations (default)
- Graph must be acyclic (enforced by invariant checks)

## Implementation

- `SuperNodeGraph`: Core graph data structure
- `TimingDomainAnalyzer`: Multi-clock domain analysis
- `SuperNodeCoarsener`: Implements coarsening strategies
- `SuperNodePartitioner`: DP-based partitioning algorithm
- `SuperNodePartitionPass`: Transform pass integration
