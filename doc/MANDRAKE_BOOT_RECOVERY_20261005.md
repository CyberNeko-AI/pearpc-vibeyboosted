# Mandrake 9.1 PPC 硬盘启动恢复记录

## 最终结果

2026-10-05，Mandrake Linux 9.1 PPC 已能从 `mandrake_hd.img` 完整进入系统。
这次恢复涉及分区表、bootstrap HFS、CHRP/yaboot 解析、initrd 存储模块和
早期 IDE 探测多个独立问题。每一步都直接作用于现有镜像；没有创建硬盘备份
或克隆。

当前磁盘布局：

| 分区 | 类型 | 起始扇区 | 大小 | 用途 |
|---|---|---:|---:|---|
| hda1 | Apple_Partition_Map | 1 | 63 | APM |
| hda2 | Apple_Bootstrap | 64 | 512000 | 250 MiB HFS bootstrap |
| hda3 | Apple_UNIX_SVR2 | 512064 | 12288000 | 6 GiB ext3 root |
| hda4 | Apple_UNIX_SVR2 | 12800064 | 2674688 | `/home` ext3 |
| hda5 | Apple_UNIX_SVR2 | 15474752 | 1302464 | swap |

hda2 与 hda3 现在连续且不重叠。根分区 `e2fsck -fn` 通过，`/home`、swap
及根分区的 ext3 特性保持不变。

## 故障链

### 1. 安装器调整 bootstrap 后留下错误 APM 范围

Mandrake 安装器曾把 hda2 的分区表长度保留为约 543 MiB，而 hda3 仍从
扇区 512064 开始。hda2 实际可用范围只有 250 MiB，分区表与文件系统布局
不一致。单独运行 `ybin` 不能安全解决这个问题。

修复方式：将 hda2 分区表长度改为 512000 个 512 字节扇区，并在确认
PearPC 已退出后重新建立 250 MiB HFS bootstrap。恢复：

- `ofboot.b`
- `yaboot`
- `yaboot.conf`
- HFS 文件类型/creator 和 bootstrap 根目录启动属性

### 2. PearPC CHRP 解析器误识别 ybin 脚本

Mandrake 的 `ofboot.b` 使用：

```forth
: bootyaboot " Loading second stage bootstrap..." .printf 100 ms
  load-base release-load-area " hd:2,\\yaboot" $boot ;
```

旧 PearPC 代码用 `strstr(buf, "boot ")` 搜索启动命令，会先命中
`bootyaboot` 的尾部，因而无法取得真实的 `hd:2,\\yaboot` 路径。

修复：加入有限 CHRP boot-script 解析器，识别独立的 `boot`、字符串后的
`$boot`，以及 `bootyaboot` 定义调用；不执行任意 Forth。真实 bootstrap
脚本和安装光盘脚本均通过测试。

### 3. initrd 缺少 CMD646 存储模块

内核 `2.4.21-0.13mdk` 配置为：

```text
CONFIG_BLK_DEV_CMD64X=m
CONFIG_EXT3_FS=m
CONFIG_JBD=m
```

根分区的 `cmd64x.o.gz` 存在，但原 initrd 只加载 `jbd.o` 和 `ext3.o`，
没有 `cmd64x.o`。因此根设备无法在 initrd 阶段正常访问。

修复：将匹配版本的 `cmd64x.o` 加入 initrd `/lib`，并在 `linuxrc` 中先加载：

```sh
echo "Loading cmd64x module"
insmod /lib/cmd64x.o
```

同时在 `/etc/modules.conf` 加入：

```text
probeall scsi_hostadapter cmd64x
```

### 4. 模块加载后旧 IDE 核心没有重新扫描通道

加载 `cmd64x` 后，内核日志只显示控制器初始化：

```text
CMD646: IDE controller at PCI slot 01:01.0
CMD646: 100% native mode on irq 26
ide0: BM-DMA at 0x1c00-0x1c07, BIOS settings: hda:pio, hdb:pio
```

但没有 hda/hdb 或分区探测记录。对照同版本 Linux 源码，模块注册路径
配置 PCI 控制器，却不一定在模块晚加载后再次调用 IDE 探测。

手动验证参数：

```text
ide0=0x1c40,0x1c32,26
```

加入后 Linux 成功识别：

```text
hda: EIN GEBUESCH!, ATA DISK drive
hdb: ZWEI GEBUESCH!, ATAPI CD/DVD-ROM drive
ide0 at 0x1c40-0x1c47,0x1c32 on irq 26
Partition check: ... p1 p2 p3 p4 p5
```

最终已把该参数写入根分区和 bootstrap 中的两个 `yaboot.conf` 副本：

```text
append=" devfs=mount ide0=0x1c40,0x1c32,26"
append=" devfs=nomount failsafe ide0=0x1c40,0x1c32,26"
```

### 5. initrd 的 `/dev/root` 仍指向 RAM 盘

即使 hda 分区已经识别，initrd 仍在挂载 ext3 时返回 error 6。现场内存和
Linux 2.4 `do_mounts.c` 对照表明，PMAC/initrd 阶段的 `real_root_dev` 仍为
RAM 盘 `0x0100`，所以 `mkrootdev /dev/root` 没有指向 hda3。

修复后的 `linuxrc` 在 `mkrootdev` 前明确设置 hda3：

```sh
echo 0x0303 > /proc/sys/kernel/real-root-dev
mkrootdev /dev/root
echo 0x0100 > /proc/sys/kernel/real-root-dev
```

后面的 `0x0100` 保留，用于告知内核根切换已由 initrd 的 `pivot_root` 完成。

## 责任边界评估

这不是单一一方的错误。

**Mandrake 侧的问题：**

- 安装器改变 bootstrap 大小时留下了不一致的 APM/HFS 布局；
- 内核把 `cmd64x` 编译成模块，却没有把它纳入实际 initrd；
- 旧 initrd/PMAC 启动流程对 `real-root-dev` 和模块晚加载有严格假设；
- 这种内核通常依赖固件/BIOS 预先配置 IDE 通道，模块本身不保证重新扫描。

**PearPC 侧仍有兼容性问题：**

- CHRP 脚本解析器原本确实有 bug，无法处理 Mandrake/ybin 常见的
  `bootyaboot ... $boot` 形式；
- PearPC 的 CMD646 模拟与这套旧 Linux 内核的模块注册/早期 IDE 扫描时序
  不一致，必须显式补充 `ide0` 基址、控制口和 IRQ；
- PROM、PCI IDE BAR、PMAC/CHRP 根设备传递没有完全复刻真实 PowerMac 固件；
- 因此即使 Mandrake 的 initrd 处理不够健壮，PearPC 仍应改进兼容性，目标是
  让标准 initrd 不需要额外的 `ide0` 参数即可探测磁盘。

当前结论是：**Mandrake 的启动介质处理不当触发了问题，PearPC 也存在真实的
固件/IDE 兼容性缺口。** 这次通过配置和镜像修复实现了可用启动，但不应把它
视为完整的 CMD646/Linux 兼容性证明。

## 后续建议

- 保留现有 `ide0` 参数，先完成系统安装和网卡调查。
- 在 PearPC 中增加针对 legacy Linux 的 CMD646 自动探测回归测试：控制器加载、
  主/从设备 IDENTIFY、PIO 首次读、APM 分区读取、hda3 根设备访问。
- 以后重建 initrd 时确保 `cmd64x` 通过 `probeall scsi_hostadapter` 或
  `mkinitrd --preload cmd64x` 进入镜像。
- 若换用其他 PowerPC 内核，优先检查其 CMD646 是否内建、是否支持模块晚加载，
  以及 `root=`/devfs 的设备命名方式。
