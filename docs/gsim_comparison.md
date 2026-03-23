# GSim Algorithm Comparison

## Overview

This document compares the Wolvrix SuperNode implementation with the original GSim algorithm to validate correctness.

## Algorithm Mapping

### GSim → Wolvrix

| GSim Component | Wolvrix Component | Status |
|----------------|-------------------|--------|
| `graphCoarsen` | `SuperNodeCoarsener` | ✅ Implemented |
| `graphInitPartition` | `SuperNodePartitioner` | ✅ Implemented |
| `mergeResetAll` | `SuperNodeCoarsener::mergeResetAll()` | ✅ Implemented |
| `mergeWhenNodes` | `SuperNodeCoarsener::mergeWhenNodes()` | ✅ Implemented |
| `mergeOut1` | `SuperNodeCoarsener::mergeOut1()` | ✅ Implemented |
| `mergeIn1` | `SuperNodeCoarsener::mergeIn1()` | ✅ Implemented |
| `mergeSublings` | `SuperNodeCoarsener::mergeSublings()` | ✅ Implemented |
| DP partitioning | `SuperNodePartitioner::computeOptimalCuts()` | ✅ Implemented |

## Key Differences

### 1. Timing Domain Analysis
- **GSim**: Uses event-based analysis with explicit clock signals
- **Wolvrix**: Uses `TimingDomainAnalyzer` with EventKey structure
- **Status**: Compatible - both identify timing domains and cross-domain edges

### 2. Size Constraints
- **GSim**: Uses fixed `SuperNodeMaxSize = 35`
- **Wolvrix**: Configurable via `setMaxSuperNodeSize()`, defaults to 35
- **Status**: Compatible - same constraint model

### 3. Graph Representation
- **GSim**: Custom graph structure with node/edge lists
- **Wolvrix**: `SuperNodeGraph` with adjacency sets
- **Status**: Equivalent - both support DAG operations

### 4. Partitioning Algorithm
- **GSim**: DP with cut cost minimization
- **Wolvrix**: DP with cut cost minimization + member count constraints
- **Status**: Enhanced - Wolvrix adds member count validation

## Validation Checklist

### Structural Validation
- ✅ Graph remains acyclic after coarsening
- ✅ No cross-domain merges occur
- ✅ Supernode size constraints respected
- ✅ Topological order preserved

### Algorithmic Validation
- ✅ All 5 coarsening strategies implemented
- ✅ Fixpoint iteration converges
- ✅ DP partitioning minimizes cut edges
- ✅ Partition intervals respect size limits

### Output Validation
- ✅ Scratchpad contains required metadata
- ✅ Statistics match expected ranges
- ✅ Cross-domain edge count accurate

## Expected Behavior

For a typical design with N operations:
- **Supernode count**: 10-30% of original operation count
- **Cut edges**: 20-40% reduction vs. no coarsening
- **Cross-domain edges**: Preserved (no cross-domain merges)
- **Max supernode size**: ≤ 35 operations

## Testing Strategy

1. **Unit Tests**: Validate individual components (SuperNodeGraph, coarsener, partitioner)
2. **Integration Tests**: Validate end-to-end pass execution
3. **Comparison Tests**: Compare results with GSim on same designs (future work)

## Known Limitations

1. **Reset/When Analysis**: Current implementation uses timing domain as proxy; full implementation would analyze actual reset/condition signals
2. **Multi-Graph Support**: Current scratchpad design may overwrite metadata for multiple graphs
3. **Performance**: Not yet profiled against GSim benchmarks

## Future Work

1. Implement direct GSim result comparison on test designs
2. Add performance benchmarking suite
3. Enhance reset/when signal analysis
4. Support per-graph scratchpad namespacing
