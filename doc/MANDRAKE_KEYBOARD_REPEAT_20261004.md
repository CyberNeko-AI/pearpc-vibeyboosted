# Mandrake 控制台按键持续重复：CUDA 完成中断

## 现象与证据

用户反馈 Mandrake Linux 9.1 安装器切换到控制台后，输入 `dmesg` 会成为
`dmesgggg...`，松开最后一个键仍持续重复；OS X 没有同样表现。
上轮增加 SDL 扫描码状态去重及失焦释放后，用户确认问题依旧。
该轮没有事件日志证明 SDL 发出了未标记的重复 KEY_DOWN，先前的归因过早。
本轮检查时没有存活的 PearPC 进程，未做客户机内存修改或重新安装。

对照 Linux 2.4 的 `drivers/macintosh/via-cuda.c`：

- `reading` 状态读最后一个字节，设置 TIP/TACK，然后进入 `read_done`。
- 到达下一次 SR 中断，`read_done` 才把完整数据包交给 `cuda_input`。
- `mac_keyb.c` 的控制台路径会在按下时安排 repeat timer，释放事件才能取消它。

本次参照源码来自上游 linux-2.4 分支（未声称已核对安装盘定制内核的全部差异）：

```text
https://kernel.googlesource.com/pub/scm/linux/kernel/git/wtarreau/linux-2.4/+/refs/heads/master/drivers/macintosh/via-cuda.c
https://kernel.googlesource.com/pub/scm/linux/kernel/git/wtarreau/linux-2.4/+/refs/heads/master/drivers/macintosh/mac_keyb.c
```

## 确认的模拟器缺陷

`cuda_write(B)` 原先在处理 TIP 上升沿前检查 `SR_INT` 并向 PIC 报告中断。
然而，传输结束时正是这个上升沿才置位 `SR_INT`。当最后的握手没有同时
改变 TACK 时，检查阶段看不到标志，因此没有发布完成中断。

寄存器级测试使用真实 `cuda.cc`、替身 PIC 和 Linux 收包顺序，在旧实现上失败：

```text
Missing IRQ in read_done/completion: state=0 left=0 B=38 IFR=04
```

这意味着 CUDA 自认为空闲、数据已读完，但 Linux 仍等待完成中断。
下一次输入可能才推动上一包完成；最后的 key-up 因此滞留，能够解释控制台
持续重复最后一个键的现象。2026-10-04 用户重启测试后确认该问题已修复。

## 修复范围

把 B 寄存器处理中的中断发布移到全部握手状态更新之后，仍以 `SR_INT` 为条件。
不改变 IFR/IER 语义，不加入超时清包，不修改按键编码或重复速率。
回退上轮未证实有效的 SDL 修改，保留原有 SDL repeat 标志过滤。

## 验证

```sh
sh test/run_cuda_keyboard_tests.sh build/a64
make -C build/a64 -j4
```

寄存器级回归测试通过 950 项检查，包括：

- `dmesg` 的全部按下/释放，最后一个释放无需下一次事件便能完成；
- TACK 初始高/低两种相位；
- CUDA autopoll、ADB 键盘寄存器查询、时钟命令/回复；
- 鼠标数据包，以及奇数/偶数字节长度的数据包。

测试未覆盖真实 PIC/CPU 中断屏蔽、CUDA 工作线程竞争或完整 OS X 初始化。
用户验证：进入 Mandrake 控制台，输入 `dmesg` 后松手等待，测试单击与长按
字母键、松手停止重复，再切回图形安装器。另用 OS X 验证键鼠输入无回归。

2026-10-04 验证记录：AArch64 构建成功，现有 16 项 headless 回归全部通过；
用户确认 Mandrake 控制台重复问题已解决。OS X 输入回归尚未单独确认。
