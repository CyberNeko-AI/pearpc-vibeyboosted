# Mandrake 根设备挂载失败及 bootstrap HFS 容量修复

## 现象与确认的原因

PROM/yaboot 已能加载 Linux，initrd 在 `Creating root device` 后报告
`mount: error 6 mounting ext3`，随后 pivot_root 失败、内核报 No init found。
不能仅凭这段日志认定 ext3 分区损坏。

2026-10-04 在 PearPC 未运行时，只读检查当前安装系统，得到：

```text
内核：2.4.21-0.13mdk
CONFIG_IDE=y
CONFIG_BLK_DEV_IDE=y
CONFIG_BLK_DEV_IDEDISK=y
CONFIG_BLK_DEV_CMD64X=m
CONFIG_EXT3_FS=m
CONFIG_JBD=m
CONFIG_MAC_PARTITION=y
```

原 `/boot/initrd-2.4.21-0.13mdk.img` 的 `/lib` 只有 `jbd.o` 和 `ext3.o`，
`linuxrc` 也只加载这两个模块，没有 CMD646 所需的 `cmd64x.o`。
匹配的模块实际存在于根分区
`/lib/modules/2.4.21-0.13mdk/kernel/drivers/ide/pci/cmd64x.o.gz`。
模块是 32 位大端 PowerPC ELF，内含 `kernel_version=2.4.21-0.13mdk`。

因此已安装内核需要模块才能访问根磁盘，却必须先访问根磁盘才找得到该模块。
缺少存储驱动的 initrd 无法提供根设备，导致本次挂载失败。
修复前对根分区运行 `e2fsck -fn` 的五项检查均通过。

## 已执行的离线修改

未创建虚拟硬盘备份或克隆。通过本机 Colima Linux 工具环境对原镜像建立
限定偏移和长度的 loop 设备；只读检查完成后，才对目标分区启用写入。

### 根分区 hda3

- 范围：字节偏移 262176768，长度 6291456000。
- 从已安装系统提取匹配版本的 `cmd64x.o.gz`，解压并放入 initrd 的
  `/lib/cmd64x.o`；其余模块与原 initrd 内容保留。
- 在 `linuxrc` 中、加载 jbd/ext3 和创建根设备之前增加：

```sh
echo "Loading cmd64x module"
insmod /lib/cmd64x.o
```

- 验证 initrd 内的 ext2，重新压缩，以原路径替换
  `/boot/initrd-2.4.21-0.13mdk.img`。
- 在 `/etc/modules.conf` 增加：

```text
# Required storage module for mkinitrd (CMD646, kernel 2.4.21-0.13mdk).
probeall scsi_hostadapter cmd64x
```

这是按磁盘中该版本 `/sbin/mkinitrd` 的实际逻辑设置的：它从 `scsi_hostadapter`
的 alias/probeall 条目收集启动所需模块，并允许模块来自 IDE 目录。
该名称不表示 CMD646 变成 SCSI 控制器。

原 initrd SHA-256：
`7691d7f3f055f8b6b5f6739fc296468933801c59a78f2be249f4467567069e57`

修复后 SHA-256：
`1c716ee455520736c54e3e05e58439898f19b0c4d8a096bbc74975c3e1c0317d`

### bootstrap 分区 hda2

- 范围：字节偏移 32768，长度 262144000（250 MiB）。
- 提取并保存本次重建必需的 `ofboot.b`、`yaboot`、`yaboot.conf` 三个文件；
  原目录仅有这三个文件，resource forks 均为 0。
- 用 hfsutils 在受长度限制的 loop 设备上重新格式化 HFS，恢复文件。
- 恢复 Finder 类型/creator：`tbxi/UNIX`、`boot/UNIX`、`conf/UNIX`，bless 根目录。
- 从新文件系统重新读出三个文件，与重建前逐字节比较一致。

新 MDB：63997 个 allocation blocks，每块 4096 字节，blessed directory ID=2。
分配区结束于分区内字节 262141440，处于 262144000 字节的分区范围内。
旧的约 543 MiB HFS 布局不再使用。

## 验证边界

- 根分区修改前后 `e2fsck -fn` 均通过。
- initrd 的 ext2 检查通过，gzip 完整性检查通过。
- 从硬盘重新提取最终 initrd，确认与暂存的新文件逐字节一致；内含 cmd64x
  也与安装系统提供的模块一致，加载顺序正确。
- PearPC 自身 HFS 读取代码仍能打开重建后的 bootstrap 引导文件。
- 镜像大小保持 8589934592 字节。
- APM 前 32768 字节的 SHA-256 不变；hda4 开始至镜像末尾（home/swap）的
  SHA-256 不变；根 ext3 的 compat/incompat/ro-compat 特性位不变。
- Linux 工具环境中的目标 loop 设备与挂载均已释放。
- 未自动启动已安装系统；挂载根设备和进入用户空间仍待用户验证。

## 第二轮：CMD646 加载后仍未探测磁盘

用户确认 CMD646 模块成功加载，但根挂载错误仍在。本轮保留的进程为 PID 18144，
只读 LLDB 采集前 32 MiB RAM 与 IDE 状态后 detach，没有改写客户机内存或磁盘。

内核 printk 缓冲（本轮映射符号 log_buf=c0249934）的有效内容显示：

```text
Kernel command line: root=/dev/hda3 ro  devfs=mount
Uniform Multi-Platform E-IDE driver Revision: 7.00beta-2.4
...
RAMDISK: Compressed image found at block 0
VFS: Mounted root (ext2 filesystem).
CMD646: IDE controller at PCI slot 01:01.0
CMD646: chipset revision 7
CMD646: 100% native mode on irq 26
    ide0: BM-DMA at 0x1c00-0x1c07, BIOS settings: hda:pio, hdb:pio
Journalled Block Device driver loaded
...
Kernel panic: No init found. Try passing init= option to kernel.
```

完全没有 hda/hdb 探测及分区日志。宿主 IDE 状态为 master installed=true、
status=0x40、current_command=0、sectorpos=0；与上一轮硬盘被禁用的情况不同。
客户机 `ide_hwifs`（c0269290）的端口已经配置为 0x1c40..0x1c47、控制口
0x1c32，但 hwgroup/gendisk 等指针仍为 0。

从同一安装盘提取 `kernel-source-2.4.21-0.13mdk.ppc.rpm`，并对照实际
`cmd64x.o` 重定位及现场内核指令：

- cmd64x 的 init_module 调用 ide_pci_register_driver。
- pre_init 结束后的注册路径调用 pci_module_init；设备回调调用
  ide_setup_pci_device，只配置控制器。
- 该路径没有调用 ide_probe_module/ideprobe_init，再扫描磁盘。
- ide_open 在 get_info_ptr 失败时直接返回 ENXIO，不能指望打开不存在的
  根设备再补做这一步。

拟验证的兼容启动参数：

```text
linux root=/dev/hda3 ro devfs=mount ide0=0x1c40,0x1c32,26
```

应在 yaboot 的 `boot:` 提示输入，而不是 PearPC 的分区选择菜单。
该内核的 ide_setup 明确支持 `ideN=base,ctl,irq`，会清除 noprobe 并在
IDE 内建驱动初始化之前设置端口。PowerMac 的 ide_init_hwif_ports 对这些
PCI I/O 端口使用连续寄存器布局，和模拟器一致。此参数只用于当前固定的
CMD646 主通道地址，尚未固化到 yaboot.conf，需用户验证 hda 和分区是否出现。

采集时还修复了 dump_kernel_log.py 对“记录前紧邻 NUL”情形的无限循环；
修复后可正常导出本次完整 printk 内容。

## 第三轮：`/dev/root` 仍指向 RAM 盘

用户现场确认 CMD646、hda 分区已经识别，但 initrd 仍报 ext3 mount error 6。
本轮内核日志出现了：

```text
hda: EIN GEBUESCH!, ATA DISK drive
hdb: ZWEI GEBUESCH!, ATAPI CD/DVD-ROM drive
ide0 at 0x1c40-0x1c47,0x1c32 on irq 26
hda: 16777216 sectors (8590 MB) ...
Partition check: ... p1 p2 p3 p4 p5
```

这排除了“硬盘没识别”与“分区不存在”。但是内核仍在 initrd 中挂载 `/dev/root`
失败。通过 System.map 和现场 RAM 交叉检查，PMAC 初始化/ initrd 阶段的
`real_root_dev` 仍为 RAM 盘设备号 `0x0100`；`mkrootdev /dev/root` 因此没有
创建 hda3 的块设备节点。

修复后的 initrd 在 `mkrootdev` 前增加：

```sh
echo 0x0303 > /proc/sys/kernel/real-root-dev
mkrootdev /dev/root
```

这里 `0x0303` 是 Linux 2.4 的 hda3 设备号（主设备 3、分区 3）。原脚本后面的
`echo 0x0100` 保留，让内核知道根切换已由 initrd 的 pivot_root 完成。

当前 initrd 已重新压缩并写入 hda3 的 `/boot/initrd-2.4.21-0.13mdk.img`；脚本
逐行确认包含该设置，根分区 e2fsck 再次通过。用户尚待重启验证 `/sysroot` 挂载
和 pivot_root 是否成功。
