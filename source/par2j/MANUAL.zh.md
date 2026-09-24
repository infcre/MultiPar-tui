# par2j Linux 版操作手册

这份手册覆盖移植后的 `par2j`（Parchive 2.0 命令行客户端，上游版本 1.3.3.7）
以及它的终端前端 `multipar-tui`。所有命令都在本机（Intel J4125，无 AVX，
4 核 Linux）实测过；标着「未实测」的项目请看第 10 节。

---

## 1. 产物与目录

| 路径 | 内容 |
|---|---|
| `source/par2j/par2j` | 原生 x86-64 ELF 二进制（约 1.4 MB），本手册的主角 |
| `source/par2j/Makefile` | 构建脚本，含 `asan` 诊断目标 |
| `source/par2j/compat/` | Win32 → POSIX 兼容层（`windows.h`、`wincompat.c`、若干桩头文件） |
| `source/par2j/test_par2j.sh` | 44 项断言、12 个场景的端到端回归测试 |
| `source/multipar-tui/` | Go 写的 TUI 前端（`README.md` 里有它自己的说明） |
| `source/multipar-tui/smoke.sh` | 前端全流程测试（含 pty 里的真实 TUI 测试） |

### 1.1 没有移植的部分（上游仓库里其他目录）

| 上游目录 | 是什么 | 为什么没接 |
|---|---|---|
| `alpha/MultiPar.exe`、`alpha/ui`、`alpha/help` | 图形界面本体 | **不在本仓库的源码里**（仓库只发布命令行核心的 C 源码），无东西可移植 |
| `source/ShellExt/` | 资源管理器右键菜单（`MultiParShlExt64.dll`） | Windows Shell 扩展 COM 组件，Linux 上对应物是 nautilus/python 扩展，得重写而非移植 |
| `source/ResourceUI/` | GUI 的多语言菜单/对话文字（.rc/.ico） | 只服务于上面那个没源码的 GUI，命令行不需要 |
| `source/par1j/`（15 文件 / 6.3k 行） | PAR **1.0** 客户端（`.par` + `.001/.p01` 分卷） | 能用现兼容层编译链接，但文件名处理还带尺寸 bug（`l` 出乱码、`r` 段错误），要单独修一轮；上游本来也不支持子目录。见 10 节 |
| `source/sfv_md5/`（15 文件 / 6.0k 行） | `.sfv` / `.md5` 校验清单的生成与核对 | 需先处理 `bcrypt.h`（Windows CNG 哈希）；且 Linux 上 `sha256sum`/`md5sum` 已能干同等的活，PAR2 场景下价值低 |

另外这些 **Windows 专项行为**在 Linux 上不存在或已降级（详见 10 节）：回收站（`-br`/`-p`
改为直接删除）、可执行文件自检（PE checksum 对 ELF 无意义）、长路径 `\\?\` 前缀、
ADS、代码页/控制台 API（改为 UTF-8）、OpenCL 驱动路径（本机无 ICD，始终跑 CPU 路径）。

---

### 1.2 跟 Linux 上已有的 PAR2 工具是什么关系（选型必读）

先说透：**PAR 2.0 是公开规范，Linux 早就有实现**，本 fork 不是在"给 Linux 带来 PAR2"。

| 需求 | 该用什么 |
|---|---|
| 只是修别人给的 `.par2` 集合 | 发行版包更省事：Debian/Ubuntu 的 `par2`（就是 **par2cmdline**，SABnzbd / nzbget 底层调的就是它），久经考验 |
| 真正做备份（增量、去重、加密、异地） | **restic / borg**。PAR2 不解决这些问题，它没有版本、没有加密、不能去重 |
| 就地冗余 + 逐位修复 + 不依赖备份端 | PAR2 的甜区（NAS 冷数据、光盘/镜像归档、下载站补块），以及**你已经有 Windows 下 MultiPar 建的存量集合** |

那这轮到底做到了什么，诚实版：

- **内核是移植，不新**。把 MultiPar 这个实现的 `par2j` 在 Linux 原生跑起来（上游只发 Windows 二进制），
  并把它整套选项面（冗余布局 / 通配过滤 / 分卷命名 / 退出码 / 多级目录）实测清楚。
- **TUI 前端是真正的增量**。就我所知 Linux 的 PAR2 生态里没有交互式终端前端
  （par2cmdline 只有裸命令行），但**这句我没联网核实过**，当选型参考可以，
  当事实引用不行。
- 程序化钩子（`PAR2J_PROGRESS` 一行一 JSON、`PAR2J_TRACE`）也是本移植新增的，方便你自己写脚本包装。
- 上面这些**全部以 `par2j` 自己的实现为前提**，与其他 PAR2 工具的互操作见 10 节最后一条。

---

## 2. 构建

```bash
cd source/par2j
make                # 约 40 秒，产出 ./par2j
make asan           # 诊断构建 → ./par2j_asan（慢 5~10 倍，之后再 make clean 再 make）
make clean
```

依赖：gcc、make、binutils 里的 `objcopy`（用来把 OpenCL 源码 `source.cl` 嵌进二进制）。
源码里有头文件内的 tentative definition，所以 Makefile 用了 `-fcommon`；
`gf16.c` 里的 AVX2 函数用 `#pragma GCC target("avx2")` 单独包裹，在没有 AVX 的
CPU 上靠运行时 `cpu_flag` 判断才进入——整文件加 `-mavx2` 会让这种机器直接 SIGILL。

前端需要 Go（本机装在 `~/.local/go`）：

```bash
export PATH="$HOME/.local/go/bin:$PATH"
cd source/multipar-tui && go build -o multipar-tui .
```

---

## 3. 三分钟上手

```bash
cd /data/backup
par2j c -ss716800 -rr100 backup.par2 *.bin   # 造恢复数据（分片 700KB，冗余 100%）
par2j v backup.par2                          # 校验：完好时打印 All Files Complete，rc=0
rm some.bin                                  # 假设丢了一个文件
par2j r backup.par2                          # 修复：成功时打印 Repaired successfully，rc=16
```

**最重要的两条**：块大小选项是 `-ss`（不是 `-s`）；修复成功的退出码是 **16** 而不是 0。

例子里的 `-rr100` 不是随手的：两个 1.6 MB 的文件在 `-ss716800` 下是 **3 片/个**，
共 6 个源片，丢一个文件就是丢 3 片；`-rr10` 只会给 1 个恢复片，无论如何都修不了。
**恢复块要够弥补丢掉的分片数**，而不是文件数，拿不准就用 `par2j t` 先算。

### 3.1 目录树与冗余级别：实测结论

目录递归、各级子目录、含空格/中文的目录名、空目录、1 字节与 0 字节文件都实测通过。
create 时文件名以**正斜杠相对路径**存入集合（如 `src/a/b/c/deep.bin`），`par2j l` 可查；
整棵树被删掉后 `par2j r` 会把各级目录和文件原样重建（空目录也会重建——这是 MultiPar
的扩展行为，PAR2 规范本身不记录目录，见 `ReadMe2_English.txt`）。

冗余用 `-rr<n>` 指定百分比，`-rr1` 到 `-rr100` 都能生成。**恢复块数量是向下取整的**，
所以小冗余在小文件集上会显得"没用"。以 200 个源片（20 个 640KB 文件，`-ss65536`）为例：

| 选项 | 恢复块 | 冗余率 | 恢复文件 | 实际能修 |
|---|---|---|---|---|
| `-rr1` | 2 | 1.00% | 1 | ~2 片 |
| `-rr3` | 6 | 2.95% | 1 | 实测修好 3 片（失 10 片则报 `Need 4 more slice(s)`，rc=12） |
| `-rr25` | 50 | 24.63% | 5 | 实测修好 10 片 |
| `-rr100` | 200 | 100% | 4–5 | 实测整树全删后完全恢复 |

结论："3% 也能生成和还原"成立，但 3% 只能救少量分片。要抗"删掉一整个文件"，
先算清那个文件有多少片，再让 `-rr` 给够块数（或直接用 `-rn<n>` 指定块数）。

---

## 4. par2j 命令行

### 4.1 语法与三条硬规则

```
par2j <子命令> [选项] <par 文件> [输入文件...]
```

1. 选项必须全部写在 `<par 文件>` **之前**，文件写在选项之后。
2. 选项前缀 `/` 和 `-` 完全等价，但**同一条命令里不能混用**（开头用了哪个，后面都必须用哪个）。
3. 选项和它的值之间**不能有空格**：`-rr10` 对，`-rr 10` 错；`-d/tmp/x` 对，`-d /tmp/x` 错。

### 4.2 子命令

| 子命令 | 用途 | 退出码 |
|---|---|---|
| `c` / `create` | 创建恢复文件 | 0 成功 |
| `v` / `verify` | 只校验，不改动任何文件 | 0 完好；132 可修复；12 修不了 |
| `r` / `repair` | 校验并修复（会写临时文件，比 v 慢） | 16 修复成功；12 无法修复 |
| `l` / `list` | 只列出集合内容，不做校验，很快 | 0 |
| `t` / `trial` | **试算**：只报分片数/恢复块数/文件大小，不写任何文件 | 0 |

### 4.3 选项表

`c` = 仅创建可用，`v/r` = 仅校验修复可用，`l` = 仅列表可用。

| 选项 | 可用 | 说明 |
|---|---|---|
| `-ss<n>` | c | **分片大小（字节）**。最常用的一个 |
| `-sn<n>` / `-sr<n>` / `-sm<n>` | c | 源块数量 / 源块比例 / 分片大小取整倍数 |
| `-rr<n>` | c | 冗余百分比，如 `-rr10` |
| `-rn<n>` | c | 直接指定恢复块数量 |
| `-rp<n>` / `-rs<n>` | c | 可恢复文件数 / 起始恢复块编号 |
| `-rd<n>` / `-rf<n>` / `-ri` | c | 恢复块如何分配到各恢复文件 / 恢复文件个数 / 用文件序号命名 |
| `-lr -lp -ls <n>` | c | 单文件恢复块数上限 / 包重复上限 / 拆分文件大小上限 |
| `-lc<n>` | 全部 | 限制 CPU 特性（诊断用） |
| `-m<n>` | 全部 | 内存用量：`-m5` = 5/8，或 `-m772`（4 + 256×3 MB） |
| `-vl<n>` | v/r | 校验级别 |
| `-vs<n>` / `-vd<dir>` | c/v/r | 复用最近校验结果 / 指定中间结果目录（缓存文件默认落在 **par2j 可执行文件旁边**，见 10 节） |
| `-d<dir>` | 全部 | **源文件所在目录**。源文件不在 par2 旁边时，创建/校验/修复都要给同一个值 |
| `-in` | c | 不生成索引文件 |
| `-up` | c | 为非 ASCII 文件名额外写 Unicode Filename packet |
| `-uo` | 全部 | 控制台输出用 UTF-8 |
| `-c"文本"` | c | 写入注释（支持中文，实测 `Comment : 中文注释😀`） |
| `-w` | v/r | 额外写一份 JSON 报告（字段见 5.6） |
| `-p` | v/r | 校验发现文件都完好时，删除恢复文件 |
| `-b` / `-br` | v/r | 修复时把原文件备份 / 送回收站（`-b` 实测把坏的 `m1.bin` 改名成 `m1.bin.1`） |
| `-bi` | v/r | 即使修复失败也替换文件 |
| `-f` / `-fu` | c/v/r | 用文件列表代替命令行文件名 / 列表按 UTF-8 编码（两者对中文名都实测可用） |
| `-fo` | c/v/r | 通配符匹配时不进子目录 |
| `-fa"*"` / `-fe"*"` | c/v/r | 白名单 / 黑名单，如 `/fa"**.zip"`、`/fe"*.par2"` |
| `-h` | l | 列出各文件的哈希值 |

### 4.4 退出码（位掩码，官方定义 + 实测）

| 码 | 含义 | 实测 |
|---|---|---|
| 0 | 正常结束（创建成功 / 校验完好 / 列表 / 试算） | ✓ |
| 1 | 致命错误（未知选项 `invalid option, -zz`、指定的 par2 不存在） | ✓ |
| 2 | 用户按 c 键取消 | — |
| 4 | 输入文件不完整（损坏或丢失） | ✓ |
| 4\|8 = 12 | 恢复块不足，修不了 | ✓ |
| 16 | **修复成功** | ✓ |
| 16\|4 = 20 | 修复失败（部分文件没修好） | — |
| 32\|4 = 36 | 可以改名 / 移动 / 还原 | — |
| 64\|4 = 68 | 可以重组 / 重建 | — |
| 128\|4 = 132 | 损坏但可以修复 | ✓ |
| 128\|8\|4 = 140 | 或许可以修复 | — |
| 256 | PAR 文件不完整 | — |

脚本里**不要**用 `[ $? -eq 0 ]` 判断成功。判"要修"用 `code & 4`，判"能修"用 `code & 128`，
判"修好了"用 `code -eq 16`。

### 4.5 路径规则

POSIX 根被映射成虚拟 `C:\` 盘。下面三种写法等价，都实测可用：

```bash
par2j v /tmp/x/y.par2
par2j v C:/tmp/x/y.par2
cd /tmp/x && par2j v y.par2        # 相对名按 par2 所在目录解析
```

命令行上非 ASCII 的路径按 **UTF-8** 解码（与 locale 无关，`LC_ALL=C` 下也正常）。
`C:\` 这类 Windows 非法字符（`< > | "`）在文件名里会被拒绝；名字里含**非 UTF-8
字节**的文件用不了，会给出明确报错而不是崩溃。

---

## 5. 常用场景配方

### 5.1 打包备份（源文件与 par2 同目录）

```bash
par2j c -ss716800 -rr10 backup.par2 *.bin
```

### 5.2 源文件与 par2 分开放

```bash
par2j c -ss200000 -rr50 -d/data/in /data/par/backup.par2 a.bin b.bin
par2j v -d/data/in /data/par/backup.par2        # 校验也要 -d
par2j r -d/data/in /data/par/backup.par2        # 修复同样要 -d
```

### 5.3 先算算要生成多大，再决定参数

```bash
par2j t -ss716800 -rr30 t.par2 *.bin     # 只报数，不写盘
```

### 5.4 只想知道集合里有什么

```bash
par2j l backup.par2                      # 列表
par2j l -h backup.par2                   # 连哈希一起
```

### 5.5 修复前留个备份

```bash
par2j r -b backup.par2                   # 坏文件改名为 xxx.1，修好的写回原名
par2j r -br backup.par2                  # 改为送回收站（Linux 下的实际行为未实测）
```

### 5.6 机器可读报告

```bash
par2j v -w -vd/tmp/mid backup.par2          # → /tmp/mid/backup.par2.json
par2j v -w -vd/tmp/mid -d/data/in backup.par2   # 源文件不在旁边时也要 -d
```

**务必带 `-vd`**：不带的话 JSON 会写到**可执行文件所在目录**（`ini_path` 默认取
exe 位置），往 `/usr/bin` 里写就是你自己的问题了。内容形如：

```json
{
"SelectedFile":"C:/tmp/man/s.par2",
"BaseDirectory":"C:/tmp/man",
"RecoveryFile":["s.par2","s.vol0+3.par2"],
"SourceFile":["m1.bin","m2.bin"],
"MissingFile":["m2.bin"]
}
```

`-w` **不支持创建（`c`）**，只对 `v`/`r` 有效。

### 5.7 中文 / emoji 文件名

直接写就行，无需额外选项。跨机器分享时建议加 `-up`（写 Unicode Filename packet，
按 PAR2 规范是 2 字节 UTF-16LE）：

```bash
par2j c -up -ss1000000 -rr20 备份.par2 中文名.bin 😀.bin
```

---

## 6. TUI 前端

```bash
cd source/multipar-tui
go build -o multipar-tui .
./multipar-tui --dir /data/backup            # 交互界面
./multipar-tui --plain --op v --par p.par2   # 不进 TUI，跑一次并打印进度流
```

键位：`↑↓/kj` 选择 · `空格` 标记 · `a` 全选 · `n` 清空 · `c` 创建 · `v` 校验 ·
`r` 修复 · `l` 列表 · `b` 分片大小 · `R` 冗余百分比 · `esc` 取消 · `q` 退出。

界面包含文件列表、带阶段名的进度条、逐文件结果表（`完整/丢失/损坏/已修复`）、
par2j 原始输出的尾巴，以及把退出码翻译成中文的结果行。创建时的恢复文件名取自
第一个被标记的文件（`doc1.bin` → `doc1.par2`）。

前端以子进程方式驱动 `par2j`，因此**磁盘格式和修复逻辑仍然只有那一个被回归
测试覆盖的实现**；`par2j` 的位置按 `--bin` → `$PAR2J_BIN` → 同目录 → `../par2j/par2j`
→ `$PATH` 的顺序解析。

---

## 7. 环境变量（移植新增，都不设也不影响任何行为）

| 变量 | 作用 |
|---|---|
| `PAR2J_TRACE=1` | 把每次路径查询（`GetFileAttributes`、`FindFirstFile`、`GetFullPathName`、`GetLongPathName`）的输入、转换结果、是否命中打到 stderr。没有 gdb 的环境下这是主要排查手段 |
| `PAR2J_PROGRESS=1` | 把每次进度更新额外以一行一个 JSON 写到 stderr，给前端读，不用去刮 `\r` 重绘的文本 |
| `PAR2J_PROGRESS_INTERVAL=<ms>` | 进度刷新间隔，20..10000（默认 1024）。只影响刷新快慢 |

事件格式：

```json
{"promille":425,"count":-1,"phase":"Creating recovery slice","file":null}
{"done":true,"phase":"Recovering slice"}
```

`promille` 0..1000（`-1` = 该步骤没有可度量的进度），`count` 是调用方的计数
（校验时是已验证的分片数），`phase` 是阶段名，`file` 属于具体文件时才非 null。
默认 1 秒一次的节流意味着小作业可能只有开头和结尾两个事件，要更密就调
`PAR2J_PROGRESS_INTERVAL`。

```bash
PAR2J_TRACE=1 par2j v backup.par2                       # 看路径解析
PAR2J_PROGRESS=1 PAR2J_PROGRESS_INTERVAL=100 par2j c ... # 看机器可读进度
```

---

## 8. 测试与自检

```bash
cd source/par2j     && ./test_par2j.sh          # 44 项，建/列/校/修全流程 + 特殊文件名
cd source/multipar-tui && ./smoke.sh            # 前端：plain 全流程 + pty 里的真实 TUI
```

- `test_par2j.sh` 全程在 `mktemp` 目录里跑，`KEEP=1 ./test_par2j.sh` 保留现场；
  其中"特殊文件名"一段**故意用 `LC_ALL=C`** 跑，专门守 locale 相关回归。
- `multipar-tui/test_tui.py` 用 pty 真的启动界面、回答终端能力查询、发按键、
  读回渲染结果并断言界面文字与磁盘结果；`TUI_DEBUG=1` 转储失败时的画面。
- `par2j` 无参数运行会打印版本、"Self-Test: not applicable to this Linux build
  (code 3)" 和用法（rc=0）。那个自检是上游对 **Windows PE 可执行文件**校验
  PE checksum，在 ELF 上不可能通过，属正常现象。

---

## 9. 故障排查

| 现象 | 原因与处理 |
|---|---|
| `input file is not found` | 源文件不在 par2 所在目录 → 校验/修复也要加 `-d<目录>`；文件名里有非 UTF-8 字节则不支持 |
| 报 `Need N more slice(s)` 却觉得应该能修 | 先数一下丢了多少片：丢的是**分片数**不是文件数。一个 6 MB 文件在 `-ss716800` 下是 9 片，而 `-rr10` 只给 10% 恢复块。用 `par2j l` 看 `Input File Slice count` 和 `Recovery Slice count` |
| 修复成功但脚本判成失败 | 退出码是 16，不是 0 |
| 路径相关的怪现象 | `PAR2J_TRACE=1` 看路径解析。历史上一个"同一目录多一个字符就成功/失败"的 bug 就是 `\\?\` 前缀插错位置吃掉了文件名 |
| 非 ASCII 名字在 cron/容器里失效 | 已修：命令行按 UTF-8 解码，不再依赖 locale。若仍异常，`PAR2J_TRACE=1` 看参数转换 |
| 想看进度但只有开始和结束 | 默认 1 秒节流；调 `PAR2J_PROGRESS_INTERVAL`，或让前端去读事件流 |

---

## 10. 已知限制（别误当已支持）

### 确认不工作 / 行为与 Windows 不同

- **`-vs` 的校验结果缓存、以及 `-w` 的 JSON 都写到 par2j 二进制所在目录**，不是 par 文件旁边。
  上游把结果目录默认取 `GetModuleFileName` 的目录：`-vs` 生成 `2_<MD5>.ini` + `2_<MD5>.bin`，
  `-w` 生成 `<exe>/x.par2.json`。直接 `make && ./par2j` 时这些会落在源码目录里，
  用 `-vd<dir>`（结尾带 `/`）把它们指走。缓存本身是**可用且安全**的（见 10.1）。
- **没有回收站**：`-br`（修复时把旧文件丢进回收站）和 `-p`（purge 删恢复文件）在
  Windows 上是先尝试进回收站、失败才硬删；这里 `DeleteItem()` 就是 `unlink`，
  删了找不回来。依赖"误删还能从回收站捞"的工作流请改用 `-b`（改名备份，已实测有效）。
- **OpenCL / GPU 路径无法在本机验证**：`lib_opencl.c` 编译进去了，`source.cl` 也用
  objcopy 嵌进去了，`LoadLibraryA("OpenCL.DLL")` 在兼容层里会回退到
  `dlopen("libOpenCL.so.1")`，但这台机器没装任何 ICD（`/etc/OpenCL/vendors` 不存在），
  `init_OpenCL()` 拿不到平台，所以走的始终是 CPU 路径。GPU 加速**未验证过**。
- `-w` 的 JSON 默认写到 **可执行文件所在目录**（`<exe>/x.par2.json`），不是 par 文件旁边。
  这是上游行为（结果目录默认取 `GetModuleFileName` 的目录），用 `-vd` 可以改。
- `*` 通配会**包含点文件**（`.hiddenrc` 会入集合）。Windows 上隐藏文件默认跳过，这里不跳过。
  备份目录时注意，或者用 `-fe'.*'` 排除。

### 10.1 看起来可疑、其实已实测确认正常

- **`-vs<n>` 校验结果复用**：3 个 100 MB 文件的集合，`par2j v -vs1` 第一次 1056 ms（建缓存），
  第二、三次 **7 ms**，不带 `-vs1` 则稳定在 ~1050 ms。更关键的是**不会误报**：把一个文件
  改写 1 个字节后，带缓存的 verify 依旧报 `Damaged file count : 1`（rc=68），随后 `r -vs1`
  正常修好（rc=16）。大目录重复校验很值得加 `-vs1 -vd<自己的缓存目录>`。
- **`-t`（trial）**：完整算出恢复块布局并打印 `Trial end`，但**不写任何文件**，适合先看开销。
- **`-vl2` / `-vl3` 的 "additional verification"**：会把同目录下**其他** par2 集合一起扫进
  `PAR File list`（`-vl0` 只列选中的一套）。同一份数据上 vl0~vl4 都能抓到内容损坏，
  差异只在于扫描范围与详细度。
- **`-c`** 注释里带非 ASCII 与 `%` 都能原样往返（`Comment : 备份 hello 100%`）。
- **`-p`** 确实会**删掉**恢复文件（实测 2 个 → 0 个）。它是有效选项，不是空转 —— 别在
  还需要恢复文件的时候加。
- `-lc<n>`（限制 CPU 特性）在选项上被接受、也能正常完成，但本机是 J4125（本来就没 AVX），
  `CPU extra` 一行不变化，无法确认它在这里真的切换了代码路径。

### 已实测通过（本表格以外的选项都跑过至少一个场景）

冗余与布局：`-sn -sr -sm -rr -rn -rp -rs -rf -ri -rd0 -rd1 -lr -lp -ls -in`（每个都确实改变了
恢复块数或恢复文件的命名/个数，`-ri` 出 `x.vol_1.par2`、`-lr2` 出 6 个文件、`-rd1` 报
`variable (base 2 until 12)`）；输入选择：`-f -fu -fo -fa -fe -d -c`（含非 ASCII 注释往返）；
修复：`-b -bi -p`；校验：`-vl0/-vl1/-vl2/-vl3/-vl4`（`-vl4` 的对齐直修确认为原地写回、不留临时文件）；
输出：`-uo -h`（`-h` 列的 MD5 与 `md5sum` 逐个一致）`-w -up -lc -m t`。
注意用了 `-in` 之后没有 `x.par2` 这个索引文件，verify/repair 要拿实际存在的卷名或通配来调。

### 仍然没测 / 没做

- **与其他 PAR2 实现的互操作未实测**（本机没装第二家实现可交叉验证）。需要特别说明的是，
  MultiPar 在这几处**超出了 PAR 2.0 规范**：多级目录是把 `src/a/b/x.bin` 这样的**相对路径直接
  写进 FileDescription 的 `FileName` 字段**；空目录会写成一条 0 字节的 `"src/空目录/"` 条目；
  非 ASCII 名字额外写 Unicode Filename packet。别的解析器读到这些可能（1）正常建出目录树，
  （2）建出一个名字里带斜杠的怪文件，（3）直接判定集合无效。**反方向也一样**：
  par2cmdline 建的集合本移植能不能正确读与修，未验证。
  所以：**集合要交给别的工具（或别人的集合你来修），先拿一个小集合试一次**。
- 非 UTF-8 字节的文件名不支持；>4 GB 单文件、多线程长时间压力、断电/满盘等异常路径未测。
- 只在 Intel J4125（无 AVX，有 SSSE3/SSE4.1/PCLMUL）上验证过；AVX2 分支能编译但本机跑不到。
- `par1j`（PAR 1.0，`.par`/`.p01`）与 `sfv_md5`（`.sfv`/`.md5`）**没有随本移植交付**。
  两者都能直接用 `par2j/compat/` 这层编译并链接成 ELF（`sfv_md5` 另需处理 `bcrypt.h`），
  但 `par1j` 的文件名处理还带着 `common1.c` 里那类 `calloc(n, 2)` 尺寸 bug：`c` 能建成，
  `l` 把 `other.log` 显示成 `"otheri\366\366\366\366"`，`r` 直接段错误（rc=139）。
  要接 PAR 1.0 旧档还得单独修一轮，且 PAR 1.0 上游本来就不支持子目录。

---

## 11. 改动清单

原有源文件 10 个被修改（`common2.c/h`、`create.c`、`gf16.c`、`ini.c`、`json.c`、
`list.c`、`par2_cmd.c`、`search.c`、`Command_par2j.txt`）；新增 `Makefile`、
`compat/`（兼容层）、`test_par2j.sh`、`MANUAL.zh.md`、根目录 `.gitignore`、
`source/multipar-tui/`。

这些改动已在本仓库的 `master` 上本地提交（编译与兼容层 / 回归测试与手册 /
TUI 前端共 3 个 commit），**没有 push，也没有动 remote**。上游不在这个
fork 的发布渠道里，要提 PR 给 Yutaka-Sawada 请先单独确认。
