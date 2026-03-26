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

### 2. key 级覆盖

`setScratchpad(key, value)` 会直接替换同名 key 的旧值。

约定上，pass 应使用稳定且带 namespace 的 key，例如：
- `supernode.<graph>.count`
- `gsim.<graph>.roots`

### 3. 显式清理

`Design` 提供三种清理方式：
- `eraseScratchpad(key)`：删除单个 key
- `eraseScratchpadNamespace(prefix)`：删除某个前缀 namespace 下的所有 key
- `clearScratchpad()`：清空整个 scratchpad

会修改图结构并可能使旧 metadata 失效的 mutating transforms，必须在自身语义下显式失效相关 namespace，而不是依赖 `PassManager` 自动清空。

### 4. clone / dryrun 隔离

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
- 同一 key 的确定性覆盖
- dryrun clone 不污染原始 `Design`
- `Design::clone()` 不继承 scratchpad
- JSON roundtrip 后 scratchpad 丢失
