# Transform Scratchpad 生命周期

## 概述

Wolvrix 的 transform scratchpad 是挂在 `grh::Design` 上的进程内元数据存储，用于在多次 `PassManager::run(...)` 之间以及 transform → emit / Python wrapper 边界之间保留分析结果。

scratchpad 的目标是承载**可复用但非 JSON 持久化**的派生 metadata，例如 supernode / gsim 之类的分析结果，而不是替代 GRH 本体。

## 生命周期规则

### 1. 同一个 `Design` 上持久化

对同一个 `Design` 实例执行 pass pipeline 后写入的 scratchpad 条目，会在后续对该 `Design` 的下一次 `PassManager::run(...)` 中继续可见。

这允许：
- analysis pass 先写 metadata
- emitter 或后续 pass 在同一 `Design` 上继续消费 metadata
- 同一 namespace 在重复运行时被确定性覆盖

### 2. key 级覆盖与命名空间

`setScratchpad(key, value)` 会直接替换同名 key 的旧值。

约定上，pass 应使用稳定且带 namespace 的 key，例如：
- `supernode.<graph>.count`
- graph-only GSim target：`gsim.<graph>.*`
- instance-path GSim target：`gsim.<graph>.path.<root$inst$...>.*`

其中 GSim 的 canonical namespace 规则是：
- graph-only 选择按目标 graph 符号命名
- instance-path 选择按 **root-qualified** 实例链命名，避免不同 root 下相同局部实例链发生别名冲突

### 3. revision 驱动的 freshness 合同

每个 `grh::Graph` 都维护单调递增的 `revision()`。

任何会改变 graph 结构、端口绑定、连接关系、符号绑定或影响派生分析结果的 mutator，都必须推进 revision。`gsim` 在写出 scratchpad metadata 时会同时记录目标 graph 的 `graph_revision`，而 `EmitGsimCpp` 在消费 metadata 时会把记录值与当前 graph revision 比对：

- revision 一致：metadata 视为仍然新鲜，可继续消费
- revision 不一致：metadata 视为 stale，emit 必须显式失败

这条 revision 检查是 stale-metadata 的主防线；显式 namespace 清理仍然保留，但只作为 hygiene，而不是唯一保护。

### 4. 显式清理

`Design` 提供三种清理方式：
- `eraseScratchpad(key)`：删除单个 key
- `eraseScratchpadNamespace(prefix)`：删除某个前缀 namespace 下的所有 key
- `clearScratchpad()`：清空整个 scratchpad

会修改图结构并可能使旧 metadata 失效的 mutating transforms，应该在自身语义下显式失效相关 namespace；但 consumer 仍必须依赖 revision 合同拒绝 stale metadata，而不是假设所有 mutator 都做了完美清理。

### 5. clone / dryrun 隔离

`Design::clone()` 不复制 scratchpad。

因此：
- Python `dryrun=True` 通过克隆 design 执行 pipeline 时，不会污染原始 `Design`
- scratchpad 不会在两个无关 `Design` 实例之间泄漏

### 5. JSON roundtrip 不保留

scratchpad 不参与 GRH JSON 序列化。

因此：
- 设计经由 store/load roundtrip 后，scratchpad 必须视为丢失
- 后续 emit 如果依赖该 metadata，必须显式失败或先显式重建 metadata

## 使用建议

- 只把可重建的派生 metadata 放进 scratchpad
- key 必须带 pass / graph namespace，避免冲突
- mutating transforms 在改写目标 graph 后，要主动失效依赖该 graph 的旧 metadata
- consumer 在读取 scratchpad 时，必须把缺失、过期、结构不匹配都当成错误处理，而不是静默回退

## 当前已验证语义

当前回归测试已经覆盖：
- 同一 `Design` 上跨 `PassManager::run(...)` 持久化
- 同一 key / namespace 的确定性覆盖
- graph-only 与 instance-path `gsim` namespace 的区分
- cross-root instance-path namespace 不会互相别名
- malformed target path 会显式失败，而不是被静默归一化
- dryrun clone 不污染原始 `Design`
- `Design::clone()` 不继承 scratchpad
- JSON roundtrip 后 scratchpad 丢失
- `EmitGsimCpp` 会拒绝缺失、结构不匹配、placeholder、namespace/path 不匹配、以及 revision 不匹配的 stale metadata
- destructive graph mutation（例如 remove/erase 路径）之后，旧 `gsim` metadata 会因 revision 变化而被拒绝
