# GSim Scratchpad Producer/Consumer Contract

## Overview

The `gsim` transform pass acts as the **producer**, writing scheduling and hypergraph metadata into the Design scratchpad. The `EmitGsimCpp` emitter acts as the **consumer**, reading this metadata to drive code generation.

## Scratchpad Namespace

All keys are prefixed with `gsim.<graph_symbol>.`, where `<graph_symbol>` is the target graph's symbol name (e.g., `gsim.top_module.`).

## Required Keys

| Key | Type | Description |
|-----|------|-------------|
| `.roots` | `vector<int64_t>` | Root operation indices (no predecessors) |
| `.event_groups` | map of `string -> vector<int64_t>` | Event group name to member op indices |
| `.event_group_names` | `vector<string>` | Ordered event group names |
| `.topology.topo_order` | `vector<int64_t>` | Topological ordering of operations |
| `.topology.predecessors` | map of `int64_t -> vector<int64_t>` | Operation predecessor graph |
| `.topology.successors` | map of `int64_t -> vector<int64_t>` | Operation successor graph |
| `.topology.classifications` | map of `int64_t -> string` | Operation kind classification |
| `.topology.op_descriptors` | `vector<string>` | Human-readable operation descriptions |
| `.schedule.kind` | `string` | Must be `"activity-v1"` |
| `.schedule.version` | `int64_t` | Must be exactly `1` |
| `.schedule.contract` | `string` | Contract description |
| `.schedule.activity_order` | `vector<string>` | Ordered activity names |
| `.schedule.activity_members` | map of `string -> vector<int64_t>` | Activity to member ops |
| `.schedule.activity_classes` | map of `string -> string` | Activity classification |
| `.hypergraph.kind` | `string` | Must be `"activity-connectivity-v1"` |
| `.hypergraph.version` | `int64_t` | Must be exactly `1` |
| `.hypergraph.contract` | `string` | Contract description |
| `.hypergraph.node_names` | `vector<string>` | Hypergraph node names |
| `.hypergraph.node_members` | map of `string -> vector<int64_t>` | Node to member ops |
| `.hypergraph.edge_names` | `vector<string>` | Hypergraph edge names |
| `.hypergraph.edge_sources` | map of `string -> string` | Edge source nodes |
| `.hypergraph.edge_targets` | map of `string -> string` | Edge target nodes |
| `.hypergraph.edge_sinks` | map of `string -> vector<int64_t>` | Edge sink ops |

## Graph Revision Check

The consumer records `graph.revision()` at metadata load time. Before emitting code, it re-checks that the graph revision matches the recorded value. If the graph was mutated after the `gsim` pass ran, the emitter rejects the stale metadata with a clear diagnostic.

## Version Contract

Both `schedule.version` and `hypergraph.version` must be exactly `1`. The consumer rejects any other value, including future versions, to prevent silent contract drift.

## Failure Modes

| Condition | Behavior |
|-----------|----------|
| Missing required key | Emit fails with "missing required gsim scratchpad key" |
| Version != 1 | Emit fails with "version mismatch" |
| Graph mutated after gsim pass | Emit fails with "stale metadata" |
| No gsim pass run | Emit fails with "missing scratchpad metadata" |
