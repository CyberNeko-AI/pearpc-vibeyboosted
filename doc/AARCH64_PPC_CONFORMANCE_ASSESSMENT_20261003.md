# AArch64 JIT 与真实 PowerPC 的一致性评估

日期：2026-10-03

本文记录当前 PearPC AArch64 JIT（目标模型为 32 位 PowerPC G3/G4，实际启动配置使用 G4/7400 类 PVR）的覆盖情况、已知差异和后续全量测试方案。这里的“真实 CPU”应理解为目标 PowerPC ISA、实现手册和目标型号的可观察行为；QEMU TCG 可以作为独立软件参考模型，但不能替代真实 7400/7410 硬件。

## 当前实现的结构

AArch64 后端是混合执行器：

- 一部分指令由 `ppc_opc_gen_*` 直接发射 AArch64 代码。
- 一部分指令由 JIT 生成调用，转入 `cpu_jitc_aarch64` 的 C++ 解释器实现。
- 另一部分设备、异常、AltiVec 和复杂访存路径通过分组解码器或宏生成的包装器间接执行。

静态审计脚本为 `scripts/debug/aarch64_jit_coverage.py`。在当前工作树运行结果为：

- 解码表/分组路径可见处理器约 280 项；
- 可直接识别为原生 AArch64 发射的约 125 项；
- 明确走 C++ 解释器回退的约 55 项；
- 其余约 100 项通过分组、宏或间接路径处理，不能仅靠函数名静态归类。

这些数字是审计指标，不是 ISA 指令总数，也不能证明语义正确。尤其要注意：`GEN_INTERPRET` 只证明调用了 PearPC 的 C++ 实现，不证明该实现等同于真实 7400。

## 已确认的主要差异和风险

### 1. 解释器与 JIT 的状态边界

JIT 包装器必须在以下指令后终止或重新分发：`rfi`、`mtmsr`、`mtsr/mtsrin`、`icbi`、异常、改变 IR/DR 的操作。否则旧翻译块可能在地址空间或权限改变后继续执行。当前代码已有多处显式 redispatch 修复，但这类问题需要系统化测试，不能只靠启动 macOS 验证。

`GEN_INTERPRET_LOADSTORE` 还必须检查 C++ 访存返回值并进入 DSI/ISI 路径；普通 `GEN_INTERPRET` 对会抛异常的操作则可能丢失“本条指令不应继续执行”的控制流。应将包装器分类纳入自动审计。

### 2. MMU、BAT、TLB 和异常模型是简化模型

当前实现是 32 位、页表和 BAT 的模拟器模型，存在以下与物理 7400 的差别风险：

- TLB 是软件缓存，淘汰、失效和 `tlbsync` 的可观察时序不等同于硬件；
- 页表遍历、权限位、键控访问、保护异常和跨页访问需要逐项验证；
- 指令地址越界、DSI/ISI 的 SRR0、SRR1、DSISR、DAR 保存时机必须按“发生异常的那条指令”核对；
- `dcbz`、cache hint、`sync/eieio` 等操作在单核模拟器中可能被简化为 no-op，但客户机仍可能通过设备访问顺序观察到差异；
- 真实 7400 的 cache、TLB、总线和外部中断时序无法由当前软件模型完全重现。

最近出现的 `entry not physical` / `code mapping outside RAM` 类故障说明地址转换和翻译块失效边界仍是高风险区，应优先做 MMU 差分测试。

### 3. 原子保留语义并非完整的硬件模型

`lwarx/stwcx.` 当前使用 CPU 状态中的软件 reservation 标记，并按再次读值判断是否成功。单线程测试已覆盖成功、无保留、值变化和异常重试，但真实 PowerPC 还涉及：

- 保留粒度（通常为 reservation granule，而不是单一字）；
- 外部写入、DMA、设备写入对保留的清除；
- 中断、异常、上下文切换和 cache 状态对保留的影响；
- 多处理器竞争和内存序。

PearPC 当前是单 CPU 模型，因此无法声称实现了完整 SMP 原子语义；至少应明确测试并记录“单 CPU、无并发 DMA”边界。

### 4. FPU/VSCR/AltiVec 的精确性仍未达到硬件级

FPU 代码包含软件浮点分类、舍入和异常逻辑；当前测试覆盖常见运算和四种舍入模式，但仍需补齐：NaN payload、信号 NaN、无穷、非规格化数、负零、FPSCR 异常标志/使能、fused multiply-add 的中间舍入和精确异常优先级。

AltiVec 大量指令仍由 C++ 解释器执行，AArch64 原生发射只覆盖小子集。VSCR 饱和位、CR6 更新、字节序、未对齐/跨页向量访存和禁用 MSR[VEC] 时的 0xF20 异常都必须单独验证。

### 5. SPR、MSR 和实现型号差异

启动时的 `read from spr 25:31 (L2CR) not supported` 表示客户机读取了 L2CR，而当前模拟器没有实现该寄存器。对 macOS 这通常只是硬件探测路径，返回值若未被依赖一般不影响启动；但它不等于真实 7400 行为。类似风险还包括 HID、ICTC、IABR、DABR、热管理寄存器和实现相关 SPR。

`PPC_CPU_UNSUPPORTED_MSR_BITS` 明确屏蔽了一些真实 MSR 位（例如 LE、ILE、BE、IP 等实现相关行为）。这符合当前单一 G4 目标的范围，但必须在测试中区分“目标型号不存在”与“尚未实现”。

### 6. 时间基准和 DEC 是宿主时钟近似

TBL/TBU、DEC 和外部中断由宿主高精度时钟和定时器驱动。它们保证了操作系统可启动，但不保证真实 CPU 的周期级行为、写 DEC 后的精确边界、TB rollover、DEC 下溢与外部中断竞争顺序。需要做容差测试，而不是按单周期比较。

## “全量测试”的可行定义

不存在一份能在不限定型号、实现和时序的情况下完全反映所有真实 PowerPC 处理器的单一测试。可执行的目标应定义为：

1. 固定 ISA/型号：32 位 PowerPC 7400/7410，含 AltiVec，遵循目标 Programming Environments Manual；
2. 对每条已解码指令覆盖所有编码合法性、寄存器别名、边界值、异常和 CR/XER/FPSCR/VSCR 副作用；
3. 用至少两个独立 oracle：PearPC generic interpreter 与 QEMU TCG；关键子集再用真实 PowerPC 硬件或 macOS/Linux PPC 实机验证；
4. 对 MMU、设备、时间和中断采用性质测试与容差，而不是逐周期相等。

## 建议的测试分层

### A. 静态覆盖审计

持续运行 `scripts/debug/aarch64_jit_coverage.py`，并把输出保存为构建产物。新增 opcode、解码表项、解释器包装器或异常路径时，要求覆盖数字和分类发生可解释变化。

同时扩展现有 `scripts/debug/check_interpreter_parity.sh`，检查：

- generic 中会抛异常的函数是否被错误地包装成 `GEN_INTERPRET`；
- load/store 是否使用带 fault 检查的包装器；
- 分支、`rfi`、`mtmsr`、`icbi` 是否结束块并从 `npc` 重新分发；
- AltiVec 禁用时是否进入 0xF20，而不是继续执行。

### B. 指令语义差分

新增一个固定状态的 runner，按随机种子生成 PPC 指令序列，在以下三种执行器中运行：

- generic interpreter；
- AArch64 JIT；
- QEMU PPC 目标（优先使用 TCG，无操作系统的裸机 harness）。

每条序列保存初始和最终的 GPR、CR、XER、FPSCR、VSCR、LR、CTR、SRR、内存窗口和异常类型。出现差异时保存最小化指令序列、种子和完整状态。

随机生成器必须遵守 PowerPC 编码约束，禁止把保留编码当作“任意合法指令”。对除法、移位、比较、CR 逻辑、更新形式访存、字节反转、FPU 舍入和 AltiVec 饱和操作使用定向边界值。

### C. MMU/异常 litmus

至少覆盖：

- IR/DR 开关与 `mtmsr` 后的下一条指令；
- BAT 命中/未命中、页表命中/缺页、权限、键控访问；
- 指令跨页、数据跨页、`lmw/stmw` 跨页和异常重试；
- DSI/ISI 的 SRR0、SRR1、DSISR、DAR；
- `rfi` 恢复 MSR、地址空间和翻译缓存；
- `icbi`, `tlbie`, `tlbia`, `tlbsync` 后的旧块不可继续执行。

现有 `test/test_dsi.S` 和 `test/test_multiple_dsi.S` 应扩展为表驱动的多页、多权限、多寄存器版本。

### D. 原子和内存序

保留现有 `test_stwcx_fragments.cc`，增加：

- reservation granule 内相邻地址写入；
- DMA/设备写入清除保留；
- 异常、中断、`sync`、`isync`、`eieio` 前后的成功率和可见顺序；
- 多线程仅用于标记当前模型不支持的情形，避免把单 CPU 结果误报为硬件一致。

### E. FPU/AltiVec 全覆盖

从 QEMU 的 PPC 指令定义表生成测试向量目录，每条指令至少包含正常值、边界值、NaN/Inf/负零、舍入模式、异常使能和 CR/VSCR 副作用。对向量指令另外覆盖 lane 顺序和大端内存布局。

### F. 系统级回归

保留当前 13 项 ELF 测试和 macOS 启动回归，但把结果分成：

- CPU 语义；
- MMU/异常；
- CUDA/输入；
- IDE/PCI/网络；
- 图形和计时。

这样可以避免“系统能进桌面”掩盖某条指令或异常路径的错误。

## 与 QEMU 的对照范围

QEMU 的 PPC TCG 翻译器和 helper 实现适合作为独立软件 oracle，重点对照：

- `target/ppc/translate.c`：解码、分支、异常和条件寄存器更新；
- `target/ppc/translate/fixedpoint-impl.c.inc`、`float-impl.c.inc`、`vector-impl.c.inc`：整数、浮点和 AltiVec 语义；
- `target/ppc/mmu_helper.c`：异常、TLB 和地址转换；
- `tests/tcg/ppc/`：PPC 用户态指令测试。

QEMU 同样是软件模型；若 QEMU 与 PearPC 一致，只能说明两套实现相互一致，不能证明完全符合物理 7400。因此测试报告应记录 oracle 版本、CPU 型号和未定义/实现相关行为。

## 优先级

1. 先完成自动静态审计和 generic/JIT 差分 runner；它们能最快定位最近出现的 JIT 包装器和块边界问题。
2. 再补 MMU/异常与 FPU/AltiVec 的定向向量；这两部分最容易出现“系统偶尔能启动但运行中崩溃”。
3. 最后引入 QEMU 和硬件对照，把实现相关 SPR、cache、时间和原子语义的边界写入测试报告，而不是把它们混入普通指令正确性结果。

## 已落地的自动化工具和首次结果

`scripts/debug/aarch64_jit_coverage.py` 现在支持 `--json`，会输出解码器安装项、原生发射项、解释器回退项、间接/未分类项、generic-only 项和包装器审计警告。当前工作树运行结果为：280 个可见处理器、125 个原生发射、55 个解释器回退、100 个间接或未分类项；本轮没有报告包装器警告。

`scripts/debug/run_differential.py` 运行相同的裸机 ELF 配置，并比较两个 CPU 构建的进程状态、退出码、超时和失败标记。用当前 AArch64 JIT 构建和从 `HEAD` 单独构建的 generic CPU 运行 15 个配置后，修复 `stwcx.` 后结果为 15 项匹配、0 项不匹配：

- `test_alu`：JIT 返回 1，generic 返回 0；JIT 输出 `test 50 stwcx stale: FAIL`。
- 其他返回 0 的 CPU 测试均匹配。
- `test_mandrake` 和 `test_prom` 在两个构建中都返回 1，属于配置/启动失败，不算 CPU 语义差异。

修复前的差异来自当前工作树对 `stwcx.` 的修改：JIT 路径不再比较 reservation 保存值，导致过期 reservation 错误写入。现已恢复 generic 与 JIT 的 reservation 值比较，并重新验证为全量 15 项匹配。

示例：

```sh
python3 scripts/debug/aarch64_jit_coverage.py --json /tmp/aarch64-audit.json
python3 scripts/debug/run_differential.py \
  --jit ./src/ppc \
  --generic /path/to/generic/src/ppc \
  --timeout 30 \
  --json /tmp/pearpc-diff.json
```

runner 会为每个配置保存独立的 JIT/generic 日志；后续可在裸机 harness 中增加寄存器和内存快照，逐条比较指令状态。
