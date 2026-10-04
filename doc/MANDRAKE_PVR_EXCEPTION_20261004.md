# Mandrake 软件包检查阶段：用户态 PVR 探测后的 JIT 异常漏跳转

## 用户报告与前序提交

硬盘容量修复已由用户确认：安装器完成分区，进入可用软件包检查。
该修复提交为 `b60b505`。随后宿主发生 machine check：

```text
pc=0f9232c0 npc=00000700 msr=00000000
srr0=0f9232bc srr1=0004f932 current_opc=7d3f42a6
backtrace: io_mem_write -> ppc_write_effective_word_slow -> .Lwrite_word_slow
```

本轮检查没有存活的 PearPC 进程，不能从原现场取得目标写地址或内存转储。
没有修改 VM 镜像，没有自动重启安装。

## 定位

`0x7d3f42a6` 解码为 `mfspr r9,287`（PVR），原 SRR1 中包含 MSR[PR] 和
程序异常的 privilege 位。`npc=0x700`、`MSR=0` 是异常已建立但尚未正确调度的状态。

`ppc_opc_mfspr` 在用户态调用 `ppc_exception(PROGRAM, PRIV)` 后返回 0。
通用解释器会执行设置后的 npc；AArch64 的 `ppc_opc_gen_interpret` 却假定
非访存回退不会触发同步异常，直接继续当前已翻译块。随后的访存因 MSR[DR]
已清除而使用错误的物理地址，产生二次 machine check。

上游 Linux 2.4 `arch/ppc/kernel/traps.c` 的 `emulate_instruction` 明确识别
PVR 读取编码 `0x7c1f42a6`（mask `0xfc1fffff`），在异常中替用户程序读取 PVR。
因此需要正确进入客户机异常处理程序，不能简单允许所有用户态 SPR 访问或
忽略非法物理地址。所核对的上游分支不是安装盘定制内核的完整源码对照：

```text
https://kernel.googlesource.com/pub/scm/linux/kernel/git/wtarreau/linux-2.4/+/refs/heads/master/arch/ppc/kernel/traps.c
```

## 复现与修复

新增裸机测试先安装 Program handler，再进入 MSR[PR,IR,DR] 用户模式。
故障指令后安排对 `0xfffffffc` 的 guard store，正确的异常处理会跳过它。

旧 AArch64 构建稳定复现与用户报告相同的模式：

```text
no one is responsible for address fffffffc ... from 00100060
pc=00100060 npc=00000700 msr=00000000
srr0=0010005c srr1=00044030 current_opc=7d3f42a6
FATAL: machine check exception
```

同一个 ELF 在已有 generic 对照构建上正常结束。

修复通用回退包装：

1. 在 CPU 状态现有 padding 内增加 `interpreter_exception`，不移动汇编依赖的
   后续成员；用 static_assert 校验异常字段区域布局。
2. 每次回退前清除该标志；C++ `ppc_exception` 建立异常后设置它。
3. 回退返回后有标志即通过 `PPC_STUB_NEW_PC` 分派 npc，否则正常继续。

不依赖旧处理函数的返回值，也不只比较 npc 是否等于 pc+4（异常向量恰好等于
顺序地址时仍需切换到新的翻译状态）。原有异步中断位、特权规则和访存错误
检查保持不变。此检查增加了回退路径上的少量指令，尚未做完整桌面性能测量。

## 验证结果

- AArch64 构建成功。
- AArch64 和已有 generic 对照构建均通过 17 项 headless 回归。
- 原生 stwcx 边界 3,072 次、分支状态 1,280 次检查通过。
- 新增通用回退边界检查 1,024 次通过：异常/无异常、前后向碎片、SRR、MSR、
  reservation、旧标志清除，以及 pc+4 与异常向量相同的情况。
- 裸机用例同时覆盖用户态 PVR/SDR1/DEC/HID0 读取、DEC/HID0 写入，及合法
  XER/VRSAVE 访问。

2026-10-04 用户重新运行后确认本轮修改成功。若后续再崩溃，应保留新的
完整控制台日志和寄存器状态，不能仅凭下一次 machine check 假设为同一问题。
