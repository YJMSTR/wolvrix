# GSim Algorithm Comparison

## Overview

This note tracks where the current Wolvrix supernode pipeline aligns with the
original GSim partition flow and where Wolvrix intentionally stays more
conservative.

## Component Mapping

| GSim Component | Wolvrix Component | Current Status |
|----------------|-------------------|----------------|
| `graphCoarsen` | `SuperNodeCoarsener` | Implemented, with Wolvrix-specific control/reset guards |
| `graphInitPartition` | `SuperNodePartitioner` | Implemented, deterministic and constraint-aware |
| `mergeResetAll` | `SuperNodeCoarsener::mergeResetAll()` | Implemented with structural control signatures |
| `mergeWhenNodes` | `SuperNodeCoarsener::mergeWhenNodes()` | Implemented with structural condition signatures |
| `mergeOut1` | `SuperNodeCoarsener::mergeOut1()` | Implemented |
| `mergeIn1` | `SuperNodeCoarsener::mergeIn1()` | Implemented |
| `mergeSublings` | `SuperNodeCoarsener::mergeSublings()` | Implemented |
| Partition metadata export | `SuperNodePartitionPass` scratchpad output | Implemented with graph/domain namespacing |

## Intentional Wolvrix Differences

### Explicit timing semantics

GSim assumes a simpler scheduling model. Wolvrix keeps explicit timing-domain
classification based on write-port event information and rejects unsupported
shared combinational logic across concrete timing domains.

### Reset/control compatibility

Wolvrix now treats sequential control structure as a hard merge constraint in
both coarsening and interval partitioning. That is stricter than a pure
same-clock heuristic and is necessary because GRH models reset/enable priority
explicitly through `updateCond` and `nextValue`.

### Namespaced scratchpad contract

Wolvrix emits metadata under `supernode.<graph>.<domain>.*` plus graph-level
discovery keys such as `supernode.<graph>.domains` and
`supernode.<graph>.cross_domain_edges`. This is the canonical contract for the
current branch.

## Current Alignment

### Structural alignment

- `SuperNodeGraph::merge()` now rejects cycle-creating contractions in both
  reachability directions.
- Topological ordering is deterministic.
- Control-sensitive sequential nodes only merge when their structural
  control/reset signatures match.
- Partitioner interval merges use the same compatibility rule as coarsening.

### Validation alignment

Targeted regressions currently cover:

- invalid vs. valid supernode contractions
- deterministic topological ordering
- same-domain timing propagation
- reset-sensitive coarsening
- reset-sensitive partitioning
- pass-level scratchpad coverage for sequential plus combinational graphs
- conservative failure on unsupported shared cross-domain logic

### Closest shape-level comparison recorded so far

On the repository's linear 4-node chain fixture, the Wolvrix partitioner with
`maxSuperNodeSize = 2` produces two 2-op intervals, reducing the graph from
4 supernodes / 3 edges to 2 supernodes / 1 edge while preserving acyclicity.
This matches the expected GSim-style outcome for a simple linear DAG under the
same interval-size limit.

## Remaining Gaps

- Cross-domain metadata and policy need broader integration coverage beyond the
  current targeted tests.
- The repository still needs broader full-tree verification and more explicit
  design-level evidence for node-count / cut-edge outcomes.
- Documentation and tests should continue converging on the namespaced
  scratchpad contract as the only supported interface.

## Validation Status

The branch has useful targeted evidence, but it should not yet be described as
fully validated against GSim. Practical comparison and broader regression
coverage are still ongoing tasks, not future placeholders.
