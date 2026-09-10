# PearPC 网络功能现状（macOS）

## 当前实现

PearPC 已包含 3Com 3C90x 和 Realtek RTL8139 网卡模拟。两者都通过
`createEthernetTunnel()` 连接宿主机网络后端，相关代码位于：

- `src/io/3c90x/3c90x.cc`
- `src/io/rtl8139/rtl8139.cc`
- `src/system/sysethtun.h`（后端抽象层）
- `src/system/osapi/posix/sysethtun.cc`（TUN/TAP 后端与后端选择）
- `src/system/osapi/posix/slirpeth.cc`（用户态 NAT 后端，基于 libslirp）

## macOS 限制与解决方案

现代 macOS 已移除早期系统中的 `tunnel.kext`，不再提供 `/dev/tun0`。
因此 PearPC 在 macOS 上默认使用**用户态 NAT 后端**（libslirp，即 QEMU
`-netdev user` 所使用的网络栈），无需 root 权限、无需内核驱动：

- 客户机通过内置 DHCP 服务器获得 `10.0.2.15/24`；
- 网关为 `10.0.2.2`，DNS 为 `10.0.2.3`（由 libslirp 代理）；
- 出站 TCP/UDP 通过宿主机普通套接字做 NAT；
- 支持入站端口转发（见下文 hostfwd）。

构建依赖：macOS 上需要 `brew install libslirp`（与 sdl3 一样，configure
阶段会通过 pkg-config 检测；缺失时 macOS 构建会直接报错）。

可以用下面的命令确认旧的 TUN 设备确实不存在（预期行为）：

```sh
ls -l /dev/tun0 /dev/tap0
```

## 配置入口

`ppccfg.example` 中已有网卡开关和 MAC 地址，例如：

```ini
pci_rtl8139_installed = 1
pci_rtl8139_mac = "52:54:00:12:34:56"
```

新增后端选择与端口转发配置：

```ini
# 以太网后端："" = 平台默认（macOS 为 nat，Linux 为 tun）
#             "nat" = 用户态 NAT（libslirp）
#             "tun" = TUN/TAP 设备（macOS 上已不可用）
pci_rtl8139_network = "nat"
pci_3c90x_network = "nat"

# hostfwd：宿主机端口 -> 客户机端口（10.0.2.15）
# 格式：tcp:<hostport>:<guestport> 或 udp:<hostport>:<guestport>
# 多个规则用逗号分隔，例如把宿主机 2222 转发到客户机 SSH：
pci_rtl8139_hostfwd = "tcp:2222:22"
```

macOS 上直接启用网卡（不写 `network` 键）即默认走 nat 后端，无需其它
配置即可上网；Linux 上默认仍走 TUN/TAP，可选 `"nat"`。

## 测试

- 后端协议自测：DHCP（DISCOVER→OFFER）、ARP、DNS 代理、出站 TCP
  （HTTP GET）、hostfwd 入站回环均已验证。
- 运行 `SLIRPETH_DEBUG=1` 可开启后端帧收发日志；
  `G_MESSAGES_DEBUG=all` 可开启 libslirp 内部调试输出。

