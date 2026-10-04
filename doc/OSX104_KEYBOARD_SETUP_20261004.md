# Tiger 10.4 欢迎向导键盘布局列表为空

日期：2026-10-04。当前状态：两个后端的 vsl/vsr 零移位错误已修复；2026-10-04 用户确认切回修复后的 G4 运行成功，本轮问题已通过用户验收。以下保留按阶段整理的调查记录。

## 用户现场与捕获限制

用户已完成安装；欢迎向导“选择您的键盘”列表为空，“显示全部”也不列出布局。
界面仍有响应，无法选中条目使“继续”可用。

第二轮启动记录：`crash-captures/20261004-154124-4486`，PID 4496，使用 AArch64 JIT、
PVR `000c0201`、512 MiB RAM、ADB 键鼠、RTL8139 NAT。
宿主观测 CPU 占用约 0.5%，没有证据显示 CPU 在之前的 mapDrainBusy 循环中忙等。

LLDB 附加失败，提示已附加但无法暂停进程。随后进程消失，捕获器记录退出码 137。
本轮没有发送终止信号；退出来源未确认。没有把这一附加失败当作客体列表为空的原因。
失败输出保存在该运行目录的 `live-debugger.txt`。

第一轮 `20261004-153659-3990` 的 guest-memory.bin 仅约 41 MiB，与配置的 512 MiB 不符；
其日志还包含 SlirpEthTunDevice::waitRecvPacket 的宿主 SIGSEGV。该文件不能当作完整 RAM。
这些接收线程/退出问题与输入源枚举的因果关系尚未建立，未据此禁用网卡。

用户要求需要重启时由用户操作；本轮没有再次启动安装或欢迎向导。

## 只读磁盘核对

硬盘 HFS+ 位于 APM 第 9 分区内的 HFS wrapper 中。宿主只读挂载失败后，直接以
只读方式解析 HFS+ catalog 和文件 data fork，未运行 fsck 或修改客体文件。
临时只读磁盘附件已经卸载。没有创建虚拟硬盘备份。

诊断提取目录：`/tmp/pearpc-tiger-20261004/`。

- HFS+ header 显示约 918,577 个 4096 字节空闲块，未见目标卷空间耗尽证据。
- `HIToolbox.rsrc`、英文 `Localized.rsrc` 和 `Roman.bundle/Contents/Resources/Roman.rsrc`
  与安装 Disc 1 对应文件逐字节一致。
- 英文 Localized.rsrc 包含 26 个 KCHR 与 23 个 uchr 资源。
- 中文、韩文等 `.keylayout` 文件存在。
- International.framework 的地区/键盘映射 plist 均可由宿主 plistlib 解析；
  `SALocaleToKeyboard.plist` 有 33 项，中文 KeyboardNames.plist 有 90 项。

核对的资源 SHA-256：

| 文件 | SHA-256（硬盘与 CD 相同） |
|---|---|
| HIToolbox.rsrc | `7c38084e6e0344015b84fad4a174d8f25b7c772d8483212aec88114865617138` |
| English.lproj/Localized.rsrc | `19ccc15b0a704b3d1f9fd0b6225bb2bb88e9a275bd04cdd6a1489dad79acdbc2` |
| Roman.rsrc | `cc173da37bfd7774a1e34a12ea1351ed26320c5b04464c33f7e8401034ad0ba6` |

这排除了已核对文件缺失/内容损坏的解释，不证明所有安装文件都完整，也不证明运行时
文件读取、缓存和解析没有问题。没有替换 plist、伪造布局列表或跳过注册向导。

## 枚举路径

从实际磁盘提取 Setup Assistant 的 IntroSection 与 SetupAssistantSupport/International
framework，核对符号和反汇编。欢迎向导使用：

`MBRegisterKeyboard -> MBLocaleGetAppropriateKeyboardsForLocale / MBLocaleGetRemainingKeyboards`

“显示全部”路径调用 `__getAllKeyboards`，该函数先调用 `TSMGetInputSourceCount`，
再逐项 `TSMCreateInputSourceRefForIndex`、`TSMGetInputSourceProperty` 并筛选类型。
因此要区分：计数为零、创建对象失败、属性/类型过滤结果异常、以及后续数组转换丢失。

当前磁盘系统版本为 10.4 build 8A428。主要预绑定 EA：

| EA | 观察意义 |
|---|---|
| `94a5c3d0` | 进入 __getAllKeyboards |
| `94a5c410` | TSMGetInputSourceCount 返回，r3 为返回状态 |
| `94a5c414` | 已将输出数量读入 r0 |
| `94a5c428` | 创建输入源对象之后 |
| `94a5c438`, `94a5c454` | 属性查询之后 |
| `94a5c46c`, `94a5c488`, `94a5c4a4` | 类型/属性筛选结果 |
| `94a5c4d0` | 枚举循环结束 |
| `94a5c558` | 输入源数组转键盘信息数组 |

这些地址只对应本次提取的 framework，不能作为其他 Tiger 版本的固定地址。

## 新诊断入口

```sh
scripts/debug/run_tiger_setup_trace.sh ppccfg.osx
```

- 使用原配置和原硬盘；无备份、无自动改写客体系统。
- 禁用继承的 Panther 原子地址过滤，改用该 Tiger framework 的 EA 观察点。
- 每次运行单独创建 capture 目录，输出 `guest-pc.csv` 与 `guest-pc.settings`。
- CSV 为 pc、lr、cr、msr 和 r0..r31；不读取客体内存或调用客体 MMU。
- 默认关闭，通过 `PEARPC_TRACE_PCS` 和 `PEARPC_TRACE_PC_FILE` 启用；最多 32 个地址，
  最多 20,000 条记录，逐行刷新。JIT 先比较运行时 EA，以免物理页别名产生误报。
- 追踪会影响生成代码布局、延迟标志的物化和运行时序，不能把打开追踪后现象消失当作修复。
- 如果回到空列表页后 CSV 不存在/没有对应函数记录，应先核对模块重定位或缓存路径，
  不能直接断言函数未被调用。

用户再次到达页面后需保留进程，读取数量及筛选结果再决定下一步修改。

## 验证

- 正常构建成功。
- 用 test_loop 的两个已知 EA 验证新探针，恰好生成两条包含 36 个字段的记录。
- 启用探针运行 test_alu 成功。
- 15 项 ELF 回归、3,072 项 stwcx 边界、1,280 项分支状态测试通过。
- 未修改 ADB 设备身份，也未宣称键盘列表问题已修复。

## 第二轮：枚举结果与下一层观察点

运行 `20261004-160312-7252`，PID 7262。CSV 共 913 条记录：

- TSMGetInputSourceCount 返回 r3=0，数量 r0=`96`（十进制 150）。
- 完成 150 次输入源创建与属性查询；其中 121 次经过最后一层类型/属性筛选。
- `__InputSourceArrayToKeyboardInfoArray` 被调用两次：一次用于地区推荐，一次用于“显示全部”。

因此不能再用“输入源枚举结果为零”解释空表，调查转向数组转换、NSMutableArray 构造与
`MBRegisterKeyboard.numberOfRowsInTableView:` 的返回值。

通过配置的 SIGTERM 转储取得了完整 512 MiB RAM（主动关闭，有 controlled-stop.txt），
没有对 guest 内存或磁盘进行修正。此镜像的 HPT 位于 `1fe00000`（2 MiB），不能沿用
Panther 的 `1fc00000`/4 MiB 假设。按 Setup Assistant 的 VSID `104c41` 可定位
`MBRegisterKeyboard` 实例 EA `003814f0`：其 ISA 指向 IntroSection 类 `000f7170`；
离线快照中 `_keyboardList` 字段（偏移 `4c`）为零。转换时的临时数组已经释放，
退出时捕获不足以确定最早何处丢失结果，需要在转换函数返回处捕获。

本次扩展脚本时发生了诊断工具问题：运行中的 shell 正在等待旧子进程；脚本被原地增补后，
旧 shell 从旧文件偏移继续读取新内容，导致再次调用模拟器、覆盖 console.log。
已关闭意外启动的 PID 8124 和旧捕获 wrapper；原始 CSV 另存为
`/tmp/pearpc-tiger-20261004/phase1-pcs.csv`，512 MiB RAM 保留。
捕获脚本现通过 `sh -c` 执行自身的内存副本，防止运行中编辑造成重复调用。
用临时假子进程验证：子进程运行期间增长脚本内容，仍只启动一次子进程。

更新后的 Tiger 脚本包含数组转换返回值、IntroSection 构造结果和行数的观察点；
还在第二次到达 `94a5c7cc`（数组转换返回处）自动保存一次：

- `probe-memory.bin`：完整 RAM；
- `probe-memory.bin.json`：PC、MSR、LR、CR、GPR、SDR1、SR 与 DBAT 原始值。

这些内容由 CPU 在线程的探针点写出，不需要调试器附加，不修改客体数据。
其他宿主 I/O 线程仍可能活动，因此不是整机停机快照。只有显式设置 snapshot 环境变量
才启用；默认运行不产生 RAM 文件。转储会短暂停顿，这是诊断开销。

下一轮使用相同命令，进入键盘页后勾选“显示全部”；若已勾选，可取消再勾选。
这会覆盖推荐列表和全部列表两条路径。

验证：在裸机 test_loop 的 `00100004` 探针处成功取得完整 128 MiB RAM，JSON 中
32 个 GPR、16 个 SR 和 PC 正确；15 项 ELF 回归通过。欢迎向导故障尚未修复。

## 第三轮：捕获字典 nil 异常，转向独立后端对照

运行 `20261004-161832-9240`，PID 9252。

- 仍枚举出 150 个输入源。
- 推荐列表的转换在第一个输入法条目中断；“显示全部”的转换先完成 92 项，
  随后在下一个输入法条目中断。反复切换显示全部会重复这一过程。
- 没有到达 `94a5c7cc` 的正常返回点，所以预设在该点的 RAM 快照没有触发。
  这不是文件写入失败，也不是应当把“没有快照”解释为未启用追踪。
- 低地址 IntroSection 观察点也会匹配内核中的同名 EA。分析时必须按 MSR.PR
  区分用户态与内核态；不能将该部分内核记录误当作 UI 的返回值。

用已配置的 SIGTERM 转储获得完整 512 MiB RAM 后关闭该进程，没有修改客体数据。
内存中找到真实运行时异常文本：

```text
Setup Assistant[78] *** -[NSCFDictionary setObject:forKey:]: attempt to insert nil value
```

通过实际 HPT（base `1fe00000`、2 MiB）与 Setup Assistant 的 VSID `104c41`
解析对象，失败路径中用于图标的对象是 **CFURL**，不是先前怀疑的裸 IconRef 数值。
两个 URL 指向：

- `/System/Library/Components/SCIM.component/Contents/Resources/simplifiedchinese_IM.tif`
- `/System/Library/Components/TCIM.component/Contents/Resources/traditionalchinese_IM.tif`

两文件都存在；宿主 Pillow 分别将它们解码为 16×16 RGBA、17×17 RGBA，均为 TIFF
LZW 压缩。欢迎向导在图标 URL 路径调用 NSImage 初始化，再把结果放入字典。
结合 nil 异常和中断位置，图标加载失败是当前重点；尚未证明具体是哪条 PPC 指令或
哪个共享组件造成失败。宿主解码成功也不能证明客体读到/处理了同样的数据。

没有替换这两个图标、修改欢迎向导或绕过布局选择。

### 独立解释器对照

为了区分 JIT 特有问题和共享实现/客体环境问题，从 HEAD `7693508` 加本地源码差异
建立独立构建：`/tmp/pearpc-tiger-generic.s4U1wG`，CPU=`generic`，UI=`sdl`。
不覆盖当前 AArch64 `src/ppc`，不创建硬盘备份。

最初参考构建在既有 `test_bat_tlb` 和 `test_bat_code` 各失败一项：

- generic BAT 翻译没有用 BL 掩码去掉 BRPN 中的块内地址位；
- BAT 写入没有使 generic 的 `effective_code_page` 缓存失效。

已修正这两个已独立验证的问题，参考构建随后通过全部 15 项 ELF 回归。这些修正只
作用于 generic 后端，**不构成 AArch64 欢迎向导空表的修复**。

捕获 wrapper 增加可选 `PEARPC_BINARY`，并将 generic dispatch 日志写入本轮目录。
实际以独立 generic 二进制运行 `test_loop.cfg --headless` 验证启动入口返回 0。

下一轮（仓库根目录）：

```sh
PEARPC_BINARY=/tmp/pearpc-tiger-generic.s4U1wG/src/ppc \
  scripts/debug/run_with_crash_capture.sh ppccfg.osx
```

使用相同硬盘、相同配置和相同语言/国家选择。解释器通常较慢，启动或动画速度不作为
本轮判据，只比较键盘列表是否出现。两后端计时方式也有差异，因此即使结果不同，仍需
继续缩小到具体指令/状态，不能把对照本身当作完全的硬件一致性证明。

## 第四轮：generic 同样复现，准备 G3 与 TIFF 路径对照

用户确认 generic 构建中的键盘列表同样为空。运行目录
`crash-captures/20261004-164113-18834`，PID 18843。

这降低了 AArch64 原生指令发射独有错误的可能性，但没有排除复制自相同实现逻辑的
AltiVec/FPU 模拟、公共设备层或客体软件状态问题。

参考解释器每 10,000 条指令记录一次，内核空闲自旋仍会大量输出，本轮日志增长到约
887 MiB。先用 SIGSTOP 暂停进程，提取出 53 条落在实际 libTIFF text 范围
`91b47000..91b8b000` 的采样记录，证明启动/UI 运行期间调用过 TIFF 库。
由于这是稀疏采样，不能用没有命中某个解码函数来断言它未执行，也不能把所有这些样本
都归于出错的两个图标。随后通过 SIGTERM/SIGCONT 保存完整 RAM 并关闭进程。

日志与分析文件保留；未修改虚拟硬盘。新 generic 源码将周期采样限制为 200,000 条，
到达后停止输出，避免日后长期空闲仍填满磁盘。已有参考二进制不因源码修改自动更新。

对下一轮 AArch64 诊断新增 `run_tiger_image_trace.sh`：

- 观察图标 URL 分支中 NSImage 初始化前后、TIFFReadDirectory、LZWDecode / LZWDecodeVector
  和 TIFFError 等实际 8A428 库地址。
- 用 `PEARPC_TRACE_USER_ONLY` 排除同名低 EA 的内核记录。
- 在第一次 TIFFError 入口触发可选 RAM 快照。如果没有到达此入口，不会强行生成快照。
- 地址来自从客体磁盘只读提取的 libTIFF / SetupAssistantSupport，不适用于任意其他版本。

下一步命令：

```sh
scripts/debug/run_tiger_image_trace.sh ppccfg.osx --cpu-pvr=0x00088302
```

`00088302` 是项目示例配置中列出的 G3 PVR。本次只通过命令行覆盖，不改配置文件；
硬盘、语言、国家选择保持一致，使用正常 AArch64 二进制，以便避免 generic 的低速。
目的是检查避开 G4/AltiVec 路径后结果是否变化，尚不能预先认定 AltiVec 就是根因。

验证：该入口以 G3 PVR 运行裸机 test_loop 返回 0；常规 15 项 ELF 回归通过。
尚未完成 G3 下欢迎向导的用户验证，当前没有针对空表的已验证修复。


## G3 成功与向量移位错误修复

用户确认 G3 运行恢复了键盘列表，之前部分图像加载缓慢/失败的现象也消失。
运行目录 `20261004-171601-22563` 中，观察到标量 LZWDecode 入口 45 次，
LZWDecodeVector 入口 0 次，图标 URL 分支正常返回；未记录到 TIFFError。
此对照本身只缩小到 G4 特有路径，不能独自证明具体指令错误。

### 独立复现

实际 Tiger libTIFF 的 `_Extract_LZW_Codes` 在 `91b4e810` 执行
`vsl v11,v11,v0`。其控制值来自当前比特偏移，合法情况下会为零。

两个后端的 `ppc_opc_vsl` 都无条件进行跨半部合并：

```cpp
VECT_D(r, 0) |= VECT_D(source, 1) >> (64 - shift);
```

当 shift=0 时，表达式右移 64 位，属于 C++ 未定义行为。在本次 arm64 优化构建中，
实际产生错误的跨半部 OR，零移位不再保持输入不变。`vsr` 的跨半部左移有同样问题。

自写的独立逐字节参考模型验证了旧 AArch64 fallback 和旧 generic 两份对象文件
都失败：例如 vsl shift=0 的首字节应为 `70`，得到 `fb`。
进一步把真实 `simplifiedchinese_IM.tif` 的压缩流前 16 字节作为输入，旧 generic
handler 把第一个 9 位编码从 256 破坏为 271。这个测试只使用原始字节和移位 handler，
不依赖 Cocoa、设备、安装状态或时钟。

这提供了具体因果路径：向量 LZW 位流提取损坏 → 图标加载失败 → 向导字典插入 nil
→ 布局列表构建中断。资源文件正确且两后端都失败，并不能排除共享/复制的 CPU 语义缺陷。

### 正式修改与测试

- generic 与 AArch64 的 `vsl/vsr` 只在 shift != 0 时进行跨半部合并。
- 不改变客体文件、键盘身份或向导逻辑。
- 两个后端各 12,288 个 shift/数据/源目标重叠用例全部通过，UBSan 无报错。
- 裸机 `test_vec_shift_zero` 覆盖解码、向量访存、正常执行路径及别名：旧 generic
  二进制返回 8，修复后两个后端均成功。
- AArch64 常规 16 项 ELF 回归通过；3,072 项 stwcx 边界和 1,280 项分支状态测试通过。

用户当前成功运行的 G3 会话未停止或修改，也没有修改 ppccfg.osx 的默认 PVR。
新 AArch64 二进制已经构建；待用户方便时，不加 G3 覆盖参数运行原配置，验证 G4
下的输入法列表和此前出错图像是否恢复。不需重新安装系统。此时的单元测试通过不等于 G4 GUI 验收；后续用户验证结果见下文。


## G4 验收与提交整理

2026-10-04，用户在收到修复版 G4 运行指令后确认运行成功。
本轮结论不再停留在 G3 规避方案：G4 修复已获用户验证。

交付内容分为向量移位语义修复、generic BAT 对照基线修正，以及默认不启用的诊断工具。
一般使用无需设置追踪环境变量。脚本中的 Tiger 库地址仍限定于已分析的 8A428，不能
无条件用于其他 OS X 版本。运行日志、RAM 转储、安装资源与虚拟硬盘不纳入源码提交。
