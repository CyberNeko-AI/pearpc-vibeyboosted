# Mandrake 安装器找不到硬盘：raw 镜像容量限制

## 现场确认

2026-10-04 用户已确认 CUDA 键盘问题修复，继续调查 IDE。
用户在 dmesg 中已看到 CMD646 控制器，所以此前“驱动没有自动匹配”的判断不准确；
SCSI 驱动选择界面不能证明 CMD646 没加载，更不能通过随便选择 SCSI 驱动解决。

本轮只读 LLDB 检查 PID 3522（`./build/a64/src/ppc ppccfg.linux`），随后 detach：

```text
gIDEState.config[0].installed = false
gIDEState.config[0].device = nullptr
gIDEState.config[1].installed = true
gIDEState.config[1].protocol = IDE_ATAPI
```

配置请求 master 使用 `mandrake_hd.img`，但模拟器启动时已禁用该硬盘；
slave 光驱正常。这与 IDE 控制器被识别、却找不到硬盘的现象一致。

镜像是此前创建的恰好 8 GiB 空盘：

```text
8589934592 bytes / 512 = 16777216 sectors
8589934592 % 516096 = 32768 bytes
```

旧 `ATADeviceFile` 要求大小必须是 `16 heads * 63 sectors * 512 bytes`
（516096 字节）的整数倍。创建镜像时没有核对这个项目特有限制，是此次
硬盘没有被启用的直接原因，并没有证据把它归因于某个 PCI/IDE 驱动回归。

## 修复

- raw 镜像接受非零、512 字节对齐的容量，保留旧的容量上限
  `65535 * 16 * 63 * 512` 字节，本轮不扩展 LBA48 支持。
- 正常容量继续以 16 heads / 63 sectors 提供完整柱面的 CHS 几何；
  小于一柱面的测试镜像使用 1 head / 1 sector，保证几何非零。
- IDENTIFY words 57–58 仍报告 CHS 容量；words 60–61 改为报告实际文件的
  LBA 扇区总数，避免少报末尾不足一柱面的扇区。
- PROM 的 `IDEDeviceFile::getSize()` 在乘法前转成 64 位，避免恰好 8 GiB
  被 32 位乘法溢出为 0。
- 更新配置示例中的镜像大小说明。

不调整当前 `mandrake_hd.img` 的大小或内容，不建立硬盘备份或克隆。
旧进程已在启动阶段禁用硬盘，源码修复必须重启后才生效。

## 验证

`test/run_ata_capacity_tests.sh` 使用独立临时空白稀疏文件测试真实 ATA 后端与
IDE 寄存器接口，未使用或复制用户磁盘。

旧 ATA 对象文件上，8 GiB 用例失败：

```text
Open 8589934592 bytes: invalid format (filesize isn't a multiple of 516096)
```

新实现通过 7,952 项检查：容量、64 位 PROM 文件长度、IDENTIFY、末扇区 PIO
写入/读回、后端文件内容和文件大小不变，以及空盘、非整扇区、超限容量拒绝。
既有光驱媒体/控制器测试也通过 10,688 项检查，AArch64 构建成功。

实际 Mandrake 安装尚待用户验证：重新启动，确认 master 不再出现
`[IO/IDE] ... disabling master` 警告，dmesg 出现硬盘设备（通常为 hda），
并能进入安装器分区页面。无需在 SCSI 列表中选择替代驱动。

## 用户验证结果

2026-10-04 用户确认修改成功，安装器已识别硬盘并完成分区。随后在检查软件包
阶段发生 CPU machine check，作为独立的 CPU/MMU 异常继续调查。
