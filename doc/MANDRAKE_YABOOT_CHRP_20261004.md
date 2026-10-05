# Mandrake 硬盘引导：CHRP 脚本解析误匹配

## 现象与只读证据

用户重新安装 Mandrake 后，从硬盘启动仍在 PROM 阶段报告：

```text
initializing initial page table at 00300000
cannot find boot file.
```

只读检查当前 `mandrake_hd.img` 的 HFS bootstrap 发现：

- `/ofboot.b`：2,978 字节，有效 CHRP 第一阶段脚本。
- `/yaboot`：149,732 字节，以 `7f 45 4c 46` 开头的 ELF。
- `/yaboot.conf`：557 字节，引用 `hd:3,/boot/vmlinuz-2.4.21-0.13mdk`
  及对应 initrd，根设备 `/dev/hda3`。

`bootfile.dump` 与第一阶段脚本对应，因此问题不能直接归为“没有安装 yaboot”。

## 直接原因

硬盘上的 ybin 引导脚本形式为：

```forth
: bootyaboot " Loading second stage bootstrap..." .printf 100 ms load-base release-load-area " hd:2,\\yaboot" $boot ;
```

随后调用 `bootyaboot`。旧 `mapped_load_chrp()` 只使用
`strstr(buf, "boot ")` 搜索入口。这个字符串首先匹配函数名 `bootyaboot`
末尾，提取到的“路径”成为后面的双引号，而不是 `hd:2,\\yaboot`。

安装光盘使用直接的 `boot cd:2,\\yaboot`，因此能启动光盘并不说明硬盘脚本
路径也正确。重复安装会生成同样的 ybin 脚本，无法解决这个 PROM 解析问题。

## 修复范围

新增小型词法识别器，支持：

- 独立单词 `boot` 后的路径与参数；
- 字面字符串后的 `$boot`；
- ybin 的 `bootyaboot` 定义及调用，跳过其他函数定义。

忽略注释、字符串中的 `boot` 和其他单词中的 `boot` 子串；拒绝缺失路径、
未结束字符串/定义和过长路径。替换原先截断/手工索引的字符串复制逻辑。

这是针对既有 CHRP/ybin 模式的有限兼容，不是完整 Forth 解释器。ybin 第一阶段
菜单不执行，直接进入已识别的 Linux loader；磁盘/光盘选择仍由 PearPC 菜单提供。
未修改 yaboot 配置、分区表或客户机文件。

## 验证

- 独立 ASan/UBSan 测试：18 个合成用例通过。
- 实际硬盘脚本和 ISO 中 `boot/ofboot.b` 分别增加一个捕获用例，均为 19 项通过。
- AArch64 构建成功，17 项 headless 回归全部通过。
- 使用当前硬盘做仅加载阶段验证：关闭测试配置的网卡/光驱，使用临时 NVRAM，
  在 LLDB 的 `ppc_cpu_run` 入口断点停下，记录 `PC=0x00200000`、`MSR=0x2030`、
  `gBootPartNum=2`。随后结束测试进程，没有执行 yaboot 或 Linux 指令。
- 硬盘容量和 mtime 未改变。2026-10-05 用户确认最终已成功进入 Linux；后续根设备修复见综合记录。

## 独立的一致性问题

当前 hda2 分区表已是 512,000 个扇区（250 MiB），与 hda3 不重叠。
但其 HFS MDB 仍报告 65,414 个 allocation blocks，每个 8,704 字节，
延续了原先约 543 MiB 的文件系统布局。该容量记录没有随分区表修正而重建。
本轮没有写入该分区；本次 PROM 失败已由脚本解析误匹配解释并验证修复。
后续仍应在 rescue 中检查/重建 bootstrap HFS，而不要仅扩大 hda2 覆盖 hda3。

上述 HFS 容量问题已在后续离线根文件系统修复中处理：bootstrap 按 250 MiB
重建，引导文件和 Finder 启动属性已恢复并校验。
详见 [根设备与 initrd 修复](MANDRAKE_ROOT_INITRD_20261004.md)。
