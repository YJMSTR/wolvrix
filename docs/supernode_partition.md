# SuperNode Partition Pass

## Overview

`SuperNodePartitionPass` builds timing-domain-scoped supernode metadata for each
graph in a Wolvrix design. The pass:

1. classifies every operation into a timing-domain bucket
2. builds one `SuperNodeGraph` per bucket
3. runs coarsening and initial partitioning
4. writes namespaced scratchpad metadata for downstream passes

The implementation is intentionally conservative today:

- shared combinational logic across concrete timing domains is rejected
- interval merges must preserve sequential control/reset compatibility
- operations outside sequential fan-in cones fall into a `combinational` bucket

## Scratchpad Contract

Scratchpad keys are graph/domain namespaced.

### Graph-level keys

- `supernode.<graph>.domains`
  - `vector<string>`
  - discovery list for emitted domain buckets, including `combinational` when present
- `supernode.<graph>.cross_domain_edges`
  - `vector<pair<OperationId, OperationId>>`
  - concrete cross-domain operation edges between distinct timing domains
  - edges involving the `combinational`, `cross_domain`, or `malformed` buckets
    are not emitted here

### Per-domain keys

For each domain in `supernode.<graph>.domains`, the pass emits:

- `supernode.<graph>.<domain>.count`
- `supernode.<graph>.<domain>.edge_count`
- `supernode.<graph>.<domain>.avg_size`
- `supernode.<graph>.<domain>.max_size`
- `supernode.<graph>.<domain>.cut_edges`
- `supernode.<graph>.<domain>.timing_domain`
- `supernode.<graph>.<domain>.topo_order`
- `supernode.<graph>.<domain>.op_to_sn`
- `supernode.<graph>.<domain>.sn_to_ops`
- `supernode.<graph>.<domain>.predecessors`
- `supernode.<graph>.<domain>.successors`

`op_to_sn` / `sn_to_ops` are expected to cover every operation assigned to that
domain bucket.

## Timing-Domain Policy

### Concrete timing domains

Register and memory write ports derive event-key-based domains from
`eventEdge` plus event operands. Latch write ports use latch-symbol-scoped
domains.

### Combinational bucket

Operations that are not assigned to a concrete timing domain by backward
propagation are still included in partition metadata under the
`combinational` bucket so the scratchpad contract covers the full supported
graph.

### Cross-domain sharing

If the same combinational operation is reached from more than one concrete
timing domain, it is classified as `cross_domain`. The current pass treats that
as unsupported and fails conservatively instead of merging or duplicating the
logic.

## Coarsening Policy

The coarsener runs these strategies to a fixpoint:

- `mergeResetAll`
- `mergeWhenNodes`
- `mergeOut1`
- `mergeIn1`
- `mergeSublings`

All merge strategies now share the same sequential control compatibility rule.
If two supernodes contain control-sensitive sequential write ports with
different control/reset signatures, they must not merge.

The signature is derived from:

- `eventEdge`
- control operands such as `updateCond`, `nextValue`, and mask/address fields
- event operands

This keeps reset-sensitive and async-sensitive behavior from collapsing during
either coarsening or interval partitioning.

## Partitioning Policy

The partitioner uses deterministic topological order as its dense DP index.
Intervals are only considered when:

- total member count is within `maxSuperNodeSize`
- all supernodes in the interval are sequential-control compatible
- the DP prefix state is reachable

If no valid partition satisfies these constraints, the pass fails instead of
producing a bogus backtrack.

## Current Validation Scope

The repository currently contains targeted regressions for:

- cycle-safe `SuperNodeGraph::merge()`
- deterministic topological sorting
- timing-domain duplicate-propagation handling
- control/reset-sensitive coarsening
- control/reset-sensitive partitioning
- pass-level scratchpad coverage for mixed sequential/combinational graphs

### Recorded validation fixtures

- Linear 4-node combinational chain with `maxSuperNodeSize = 2`
  - before partition: 4 supernodes, 3 edges
  - after partition: 2 supernodes, 1 edge
  - stable layout: two 2-op intervals on repeated runs
- Mixed sequential/combinational scratchpad fixture
  - proves namespaced discovery keys
  - proves total graph coverage via `op_to_sn`
  - proves graph-level `cross_domain_edges` stays empty in the single-domain case
- Shared cross-domain combinational logic fixture
  - proves the current conservative failure path raises diagnostics instead of
    silently partitioning unsupported multi-domain sharing

Broader design-level validation and performance evidence still need to be
expanded further.
