# Round 4 Implementation Status

## Critical Issues from Codex Review

### 1. SuperNodeGraph Merge Semantics (FIXED)
- ✅ Fixed cycle detection to allow edge contraction for mergeOut1/mergeIn1
- ✅ hasCircularDependency() already returns boolean correctly
- ⏳ Need to add topologicalOrder recomputation method

### 2. Invalid Structured Binding Loop (CRITICAL)
- ❌ Line 73, 84, 96 in supernode_partition_pass.cpp use invalid loop
- nodes() returns `const std::vector<SuperNode>&`, not a map
- Need to iterate over validNodeIds() instead

### 3. Incomplete Scratchpad Contract (CRITICAL)
- ❌ Missing required keys: op_to_sn, sn_to_ops, predecessors, successors, topo_order, timing_domain
- ❌ cross_domain_edges should be edge list, not count
- ❌ No per-graph namespacing (later graphs overwrite earlier)

### 4. TimingDomainAnalyzer Issues (CRITICAL)
- ❌ Treats kLatchWritePort like event-key root (should be separate domain)
- ❌ Doesn't reject malformed write ports with missing eventEdge
- ❌ Uses global visited set (nondeterministic multi-domain results)
- ❌ Still assigns unclaimed ops to "comb"

### 5. Per-Domain Processing Missing (CRITICAL)
- ❌ Pass builds one global SuperNodeGraph instead of per-domain
- ❌ No flatten precondition check (kInstance)
- ❌ No blackbox precondition check (kBlackbox)
- ❌ No diagnostics or result.failed paths

### 6. Placeholder Coarsening Rules (CRITICAL)
- ❌ mergeResetAll groups by timingDomain, not actual reset structure
- ❌ mergeWhenNodes hashes predecessors, not kMux conditions
- ❌ Never uses graph_ to inspect GRH operations

### 7. Partitioner Validation Missing (IMPORTANT)
- ❌ No explicit acyclicity validation upfront
- ❌ Recomputes topological order on every cost query
- ❌ No cut sequence validation
- ❌ No metadata recomputation after merges

### 8. Test Coverage Inadequate (IMPORTANT)
- ❌ test_supernode.cpp is small smoke test only
- ❌ No pass-manager tests
- ❌ No fixture-backed tests
- ❌ tests/transform/data doesn't exist

### 9. No Validation Evidence (IMPORTANT)
- ❌ No small/medium/large design validation
- ❌ No GSim comparison
- ❌ No AC-7 performance metrics

## Implementation Priority

1. Fix invalid structured binding loop (blocks compilation)
2. Add topologicalOrder recomputation
3. Rebuild TimingDomainAnalyzer with correct GRH contract
4. Refactor pass for per-domain processing
5. Complete scratchpad contract
6. Implement GRH-driven coarsening rules
7. Add partitioner validation
8. Extend test coverage
9. Produce validation evidence
