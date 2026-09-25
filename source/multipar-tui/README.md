# multipar-tui

par2j 的终端前端（TUI）。它**驱动 `par2j` 子进程**，自己不链接任何修复代码：
磁盘上的 PAR2 格式、GF16 运算、路径处理仍然只有 `par2j` 一个实现，也就是回归
测试覆盖的那一个。前端只负责收集参数、启动进程、读进度、把结果翻译成人话。

定位说明：Linux 上已有成熟的 PAR2 命令行实现（par2cmdline，发行版里叫 `par2`），
单纯修集合用那个更省事。本前端的价值是**交互式的集合浏览 / 选文件 / 选冗余并跑
par2j**，这在前者是缺的；另外它只驱动 MultiPar 系的 `par2j`，与其他实现的
互操作未实测（见 `source/par2j/MANUAL.zh.md` 第 10 节）。

```
multipar-tui                               # 在当前目录进入 TUI
multipar-tui --dir /data/backup            # 指定工作目录
multipar-tui --plain --op v --par p.par2   # 不进 TUI，跑一次并打印进度流
```

`par2j` 的位置按以下顺序解析：`--bin` → `$PAR2J_BIN` → 与本程序同目录的
`par2j` → `../par2j/par2j` → `$PATH`。显式给出的相对路径会被转成绝对路径，
因为子进程的工作目录是 `--dir`，而不是你敲命令时所在的位置。

## 构建

```bash
# Go 1.24+；依赖 bubbletea、lipgloss
CGO_ENABLED=0 go build -trimpath -ldflags="-s -w" -o multipar-tui .
```

`CGO_ENABLED=0` 不是可有可无的。Go 在 Linux 上默认开 cgo，产物会带
`interpreter /lib64/ld-linux-x86-64.so.2`，到 musl 系统（OpenWrt、Alpine）上就是
「打不开」。关掉 cgo 后 `file` 报 `statically linked`，且不依赖构建机的 glibc 版本
（本项目自己用不到 DNS/用户查询那些需要 NSS 的路径）。`-s -w` 只为压发布体积，
本地调试可以去掉。

## 键盘

| 键 | 作用 |
|---|---|
| ↑ ↓ / k j | 选择文件 |
| 空格 | 标记 / 取消标记（创建时的输入文件） |
| a / n | 全选 / 清空标记 |
| c | 用已标记的文件创建恢复文件 |
| v / r / l | 校验 / 修复 / 列表（用的是选中的 `.par2`） |
| b / R | 切换分片大小（每片字节数）/ 冗余百分比 |
| esc | 取消正在运行的作业 |
| q | 退出 |

创建时的恢复文件名取自第一个被标记的文件（`doc1.bin` → `doc1.par2`）。
校验和修复不需要标记任何东西：par2j 会自己找到集合里的文件。

## 退出码

`par2j` 返回的是**位掩码**，不是 0/1。前端把它翻译成一句话：

| 码 | 含义 |
|---|---|
| 0 | 正常结束，无需修复（创建成功 / `All Files Complete`） |
| 1 | 致命错误 |
| 2 | 被用户取消 |
| 4 | 输入文件不完整 |
| 8 \| 4 = 12 | 恢复块不足，修不了 |
| 16 | 修复成功 |
| 16 \| 4 = 20 | 修复失败（部分文件没能修好） |
| 32 \| 4 = 36 | 可以改名 / 移动 / 还原 |
| 64 \| 4 = 68 | 可以重组 / 重建 |
| 128 \| 4 = 132 | 损坏但可以修复 |
| 128 \| 8 \| 4 = 140 | 或许可以修复 |
| 256 | PAR 文件不完整 |

脚本里**不要**写 `[ $? -eq 0 ]` 判成功——修复成功是 16。

## 进度流

TUI 不解析 `par2j` 那些用 `\r` 重绘给人看的百分比，而是读它的 JSON 事件流：
`par2j` 在 `PAR2J_PROGRESS` 被设置时，把每次进度更新额外写到 stderr，一行一个
对象（不设置时输出与原来逐字节相同）：

```json
{"promille":425,"count":-1,"phase":"Creating recovery slice","file":null}
{"done":true,"phase":"Recovering slice"}
```

- `promille`：0..1000，`-1` 表示该步骤没有可度量的进度
- `count`：调用方的计数（校验时是已验证的分片数），`-1` 表示没有
- `phase`：这个百分比属于哪个阶段（`Computing file hash`、`Creating recovery
  slice`、`Computing matrix`、`Recovering slice`……）；空串表示未知
- `file`：属于某个具体文件时给出文件名，否则 `null`

更新频率默认是 1 秒一次（上游的 `UPDATE_TIME`），对几分钟以上的作业够用；
前端想要更顺滑可以设 `PAR2J_PROGRESS_INTERVAL=100`（毫秒，20..10000），只影响
刷新的快慢，不改变任何行为。TUI 默认就用 100ms。

`par2j` 自己报告的表头（`Input File Slice count`、`Recovery Slice found` 等）和
逐文件状态表（`= Complete` / `- Missing` / `Repaired`）由前端从 stdout 解析，
用来画进度条分母和结果列表。

## 测试

```bash
./smoke.sh                  # 构建 + plain 全流程 + pty 里的 TUI 全流程
PAR2J_BIN=/path/par2j ./smoke.sh
```

- `smoke.sh`：在临时目录里通过 `--plain` 跑 创建 → 校验 → 删文件 → 校验（应得
  132）→ 修复（应得 16）→ 复核 + `md5sum -c`；有 python3 时再跑下面的 pty 测试。
- `test_tui.py`：用 pty 真的启动 TUI，回答终端能力查询（`ESC]11;?`、
  `ESC[6n`），发按键，读回渲染结果，断言界面上出现的内容与磁盘上的结果
  （创建的文件、修复后的 md5）。`TUI_DEBUG=1` 会转储失败时的画面。

## 已知限制

- 只驱动 `par2j`。`par1j`（PAR 1.0）和 `sfv_md5`（SFV/MD5）还没有原生移植，
  所以 `.par`/`.sfv`/`.md5` 暂时接不进来——它们的源码结构与 par2j 相似，可以
  复用同一套 `par2j/compat/` 兼容层，接进来后同一个前端就能统一处理四种格式。
- 默认 1 秒的进度节流（见上）意味着很小的作业只有开头和结尾两个事件。
- `-vs` 的 INI 校验结果复用实际不生效（`recent_data` 恒为 0），JSON 输出
  （`-w`）、备份/回收站（`-b/-br/-bi/-p`）、多卷布局、OpenCL 路径都没有接进界面。
- 前端假设 `par2j` 在 `--dir` 里能找到源文件；源文件在别处时要用 `-d`，这个
  选项目前没有暴露成按键。
