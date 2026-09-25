package main

import (
	"context"
	"fmt"
	"strconv"
	"strings"
	"time"

	tea "github.com/charmbracelet/bubbletea"
	"github.com/charmbracelet/lipgloss"
)

type (
	lineMsg  string
	eventMsg Event
	doneMsg  struct {
		code int
		err  error
	}
	tickMsg time.Time
)

var (
	blockPresets = []int64{0, 262144, 524288, 716800, 1048576, 2097152, 4194304}
	redPresets   = []int{5, 10, 15, 20, 30, 50}

	styleTitle   = lipgloss.NewStyle().Bold(true).Foreground(lipgloss.Color("39"))
	styleDim     = lipgloss.NewStyle().Foreground(lipgloss.Color("244"))
	styleSel     = lipgloss.NewStyle().Background(lipgloss.Color("236")).Bold(true)
	styleMark    = lipgloss.NewStyle().Foreground(lipgloss.Color("42"))
	styleOK      = lipgloss.NewStyle().Bold(true).Foreground(lipgloss.Color("42"))
	styleBad     = lipgloss.NewStyle().Bold(true).Foreground(lipgloss.Color("203"))
	styleErr     = lipgloss.NewStyle().Foreground(lipgloss.Color("203"))
	styleBarFull = lipgloss.NewStyle().Foreground(lipgloss.Color("39"))
	styleBarRest = lipgloss.NewStyle().Foreground(lipgloss.Color("238"))
)

type model struct {
	bin string
	dir string

	entries []Entry
	cursor  int
	marked  map[string]bool

	blockSize  int64
	redundancy int

	running  bool
	op       string
	phase    string
	promille int
	count    int
	started  time.Time
	elapsed  time.Duration
	cancel   context.CancelFunc

	snap Snapshot

	log      []string
	code     int
	verdict  string
	ok       bool
	finished bool
	note     string

	width, height int
	msgs          chan tea.Msg
}

func newModel(bin, dir string) *model {
	return &model{
		bin:        bin,
		dir:        dir,
		marked:     map[string]bool{},
		blockSize:  716800,
		redundancy: 10,
		width:      100,
		height:     30,
		msgs:       make(chan tea.Msg, 512),
	}
}

func (m *model) send(msg tea.Msg) { m.msgs <- msg }

func (m *model) wait() tea.Cmd {
	ch := m.msgs
	return func() tea.Msg { return <-ch }
}

func (m *model) Init() tea.Cmd {
	m.reload()
	return m.wait()
}

func (m *model) reload() {
	entries, err := ListDir(m.dir)
	if err != nil {
		m.note = err.Error()
		return
	}
	m.entries = entries
	if m.cursor >= len(entries) {
		m.cursor = 0
	}
}

func (m *model) Update(msg tea.Msg) (tea.Model, tea.Cmd) {
	switch msg := msg.(type) {
	case tea.WindowSizeMsg:
		// a pty without a size reports 0x0; keep the defaults in that case
		if msg.Width > 0 {
			m.width = msg.Width
		}
		if msg.Height > 0 {
			m.height = msg.Height
		}
		return m, nil

	case lineMsg:
		line := string(msg)
		m.snap.Feed(line)
		if strings.TrimSpace(line) != "" {
			m.log = append(m.log, line)
			if len(m.log) > 400 {
				m.log = m.log[len(m.log)-400:]
			}
		}
		return m, m.wait()

	case eventMsg:
		if msg.Phase != "" {
			m.phase = msg.Phase
		}
		if msg.Promille != nil {
			m.promille = *msg.Promille
		}
		if msg.Count != nil {
			m.count = *msg.Count
		}
		return m, m.wait()

	case doneMsg:
		m.running = false
		m.finished = true
		m.code = msg.code
		m.verdict, m.ok = DescribeExit(msg.code)
		if m.code == 0 {
			switch Op(m.op) {
			case OpCreate:
				m.verdict = "创建成功"
			case OpList:
				m.verdict = "列表已输出"
			}
		}
		if msg.err != nil {
			m.note = msg.err.Error()
		}
		m.reload()
		return m, nil

	case tickMsg:
		if m.running {
			m.elapsed = time.Since(m.started)
			return m, tick()
		}
		return m, nil

	case tea.KeyMsg:
		return m.key(msg)
	}
	return m, nil
}

func (m *model) key(k tea.KeyMsg) (tea.Model, tea.Cmd) {
	switch k.String() {
	case "q", "ctrl+c":
		if m.running && m.cancel != nil {
			m.cancel()
		}
		return m, tea.Quit

	case "esc":
		if m.running && m.cancel != nil {
			m.cancel()
			m.note = "已请求取消"
		}
		return m, nil

	case "up", "k":
		if m.cursor > 0 {
			m.cursor--
		}
	case "down", "j":
		if m.cursor+1 < len(m.entries) {
			m.cursor++
		}
	case "home", "g":
		m.cursor = 0
	case "end", "G":
		m.cursor = len(m.entries) - 1

	case " ":
		if len(m.entries) > 0 {
			name := m.entries[m.cursor].Name
			m.marked[name] = !m.marked[name]
		}
	case "a":
		for _, e := range m.entries {
			if !e.IsPar {
				m.marked[e.Name] = true
			}
		}
	case "n":
		m.marked = map[string]bool{}

	case "b":
		m.blockSize = nextInt64(blockPresets, m.blockSize)
	case "R":
		m.redundancy = nextInt(redPresets, m.redundancy)

	case "c":
		return m, m.start(OpCreate)
	case "v":
		return m, m.start(OpVerify)
	case "r":
		return m, m.start(OpRepair)
	case "l":
		return m, m.start(OpList)
	}
	return m, nil
}

func nextInt64(list []int64, cur int64) int64 {
	for i, v := range list {
		if v == cur {
			return list[(i+1)%len(list)]
		}
	}
	return list[0]
}

func nextInt(list []int, cur int) int {
	for i, v := range list {
		if v == cur {
			return list[(i+1)%len(list)]
		}
	}
	return list[0]
}

func (m *model) markedList() []string {
	var out []string
	for _, e := range m.entries {
		if m.marked[e.Name] && !e.IsPar {
			out = append(out, e.Name)
		}
	}
	return out
}

func (m *model) selectedPar() string {
	if len(m.entries) == 0 {
		return ""
	}
	if e := m.entries[m.cursor]; e.IsPar {
		return e.Name
	}
	for _, e := range m.entries {
		if e.IsPar {
			return e.Name
		}
	}
	return ""
}

func (m *model) buildJob(op Op) (Job, error) {
	if op == OpCreate {
		inputs := m.markedList()
		if len(inputs) == 0 {
			return Job{}, fmt.Errorf("先用空格标记要打包的文件")
		}
		base := inputs[0]
		if i := strings.LastIndex(base, "."); i > 0 {
			base = base[:i]
		}
		return Job{
			Op:         op,
			ParFile:    base + ".par2",
			Inputs:     inputs,
			BlockSize:  m.blockSize,
			Redundancy: m.redundancy,
		}, nil
	}
	par := m.selectedPar()
	if par == "" {
		return Job{}, fmt.Errorf("先选中一个 .par2 文件")
	}
	// the slice size of an existing set is read from the par2 files, so -ss is
	// create only; passing it to verify or repair would be meaningless
	return Job{Op: op, ParFile: par}, nil
}

func (m *model) start(op Op) tea.Cmd {
	job, err := m.buildJob(op)
	if err != nil {
		m.note = err.Error()
		return nil
	}
	h, err := StartJob(context.Background(), m.bin, m.dir, job, 100,
		func(line string) { m.send(lineMsg(line)) },
		func(ev Event) { m.send(eventMsg(ev)) },
		func(code int, err error) { m.send(doneMsg{code: code, err: err}) })
	if err != nil {
		m.note = err.Error()
		return nil
	}
	m.running = true
	m.cancel = h.Cancel
	m.op = string(op)
	m.started = time.Now()
	m.elapsed = 0
	m.phase, m.promille, m.count = "", 0, 0
	m.snap = Snapshot{}
	m.log = nil
	m.code, m.verdict, m.ok, m.finished = 0, "", false, false
	m.note = fmt.Sprintf("%s %s", opName(op), strings.Join(append([]string{job.ParFile}, job.Inputs...), " "))
	return tea.Batch(m.wait(), tick())
}

func tick() tea.Cmd {
	return tea.Tick(time.Second/4, func(t time.Time) tea.Msg { return tickMsg(t) })
}

func opName(op Op) string {
	switch op {
	case OpCreate:
		return "创建"
	case OpVerify:
		return "校验"
	case OpRepair:
		return "修复"
	case OpList:
		return "列表"
	}
	return string(op)
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

func (m *model) View() string {
	var b strings.Builder

	head := styleTitle.Render("MultiPar TUI")
	b.WriteString(head + styleDim.Render("  "+m.dir) + "\n")
	b.WriteString(m.viewFiles())
	b.WriteString("\n")
	b.WriteString(m.viewProgress())
	b.WriteString("\n")
	if s := m.viewStatuses(); s != "" {
		b.WriteString(s)
		b.WriteString("\n")
	}
	if s := m.viewLog(); s != "" {
		b.WriteString(s)
		b.WriteString("\n")
	}
	b.WriteString(m.viewResult())
	b.WriteString(m.viewHelp())
	return b.String()
}

func (m *model) viewFiles() string {
	const reserved = 18
	rows := m.height - reserved
	if rows < 3 {
		rows = 3
	}
	if rows > len(m.entries) {
		rows = len(m.entries)
	}
	top := 0
	if m.cursor >= rows {
		top = m.cursor - rows + 1
	}
	var b strings.Builder
	fmt.Fprintf(&b, "%s %s\n",
		styleDim.Render("文件"),
		styleDim.Render(fmt.Sprintf("(%d 个, %d 已标记)", len(m.entries), len(m.markedList()))))
	for i := top; i < top+rows && i < len(m.entries); i++ {
		e := m.entries[i]
		box := "[ ]"
		if m.marked[e.Name] {
			box = "[x]"
		}
		cur := "  "
		if i == m.cursor {
			cur = "> "
		}
		line := fmt.Sprintf("%s%s %10s  %s", cur, box, humanSize(e.Size), e.Name)
		switch {
		case i == m.cursor:
			line = styleSel.Render(line)
		case m.marked[e.Name]:
			line = styleMark.Render(line)
		case e.IsPar:
			line = styleDim.Render(line)
		}
		b.WriteString(line + "\n")
	}
	return b.String()
}

func (m *model) viewProgress() string {
	var b strings.Builder
	label := m.phase
	if label == "" {
		if m.running {
			label = opName(Op(m.op))
		} else {
			label = "空闲"
		}
	}
	right := ""
	switch {
	case !m.running && !m.finished:
		right = styleDim.Render("b 分片 " + humanSize(m.blockSize) + "   R 冗余 " + fmt.Sprint(m.redundancy) + "%")
	case m.promille >= 0 && m.promille > 0:
		right = fmt.Sprintf("%5.1f%%", float64(m.promille)/10)
	case m.count > 0:
		right = fmt.Sprintf("%d 片", m.count)
	}
	pad := m.width - lipgloss.Width(label) - lipgloss.Width(right) - 8
	if pad < 1 {
		pad = 1
	}
	fmt.Fprintf(&b, "%s %s%s%s\n", styleDim.Render("进度"), label, strings.Repeat(" ", pad), right)

	filled, width := 0, m.width-24
	if width < 10 {
		width = 10
	}
	switch {
	case m.running && m.promille > 0:
		filled = m.promille * width / 1000
	case m.finished:
		if m.ok {
			filled = width
		}
	case m.count > 0 && m.snap.SliceCount > 0:
		filled = m.count * width / m.snap.SliceCount
	}
	if filled > width {
		filled = width
	}
	fmt.Fprintf(&b, "[%s%s]", styleBarFull.Render(strings.Repeat("█", filled)),
		styleBarRest.Render(strings.Repeat("░", width-filled)))

	var foot []string
	if m.running {
		foot = append(foot, "已用 "+m.elapsed.Truncate(time.Second).String())
	}
	if m.count > 0 {
		if m.snap.SliceCount > 0 {
			foot = append(foot, fmt.Sprintf("片 %d/%d", m.count, m.snap.SliceCount))
		} else {
			foot = append(foot, fmt.Sprintf("片 %d", m.count))
		}
	}
	if m.snap.RecoveryFound > 0 {
		foot = append(foot, fmt.Sprintf("恢复块 %d/%d", m.snap.RecoveryFound, m.snap.RecoveryCount))
	}
	if len(foot) > 0 {
		b.WriteString("  " + styleDim.Render(strings.Join(foot, " · ")))
	}
	b.WriteString("\n")
	if m.note != "" {
		b.WriteString(styleDim.Render("   "+truncate(m.note, m.width-4)) + "\n")
	}
	return b.String()
}

func (m *model) viewStatuses() string {
	if len(m.snap.Statuses) == 0 {
		return ""
	}
	var b strings.Builder
	b.WriteString(styleDim.Render("报告") + "\n")
	rows := m.snap.Statuses
	if len(rows) > 8 {
		rows = rows[len(rows)-8:]
	}
	for _, r := range rows {
		style := styleOK
		low := strings.ToLower(r.Status)
		if strings.Contains(low, "damage") || strings.Contains(low, "missing") || strings.Contains(low, "lost") {
			style = styleBad
		}
		fmt.Fprintf(&b, "  %s %s\n", style.Render(fmt.Sprintf("%-8s", statusLabel(r.Status))),
			truncate(r.Name, m.width-20))
	}
	return b.String()
}

// viewLog shows the tail of par2j's own report, which carries the verdict lines
// ("All Files Complete", "Repaired successfully") verbatim.
func (m *model) viewLog() string {
	var rows []string
	for i := len(m.log) - 1; i >= 0 && len(rows) < 3; i-- {
		s := strings.TrimSpace(m.log[i])
		if s == "" || isProgressNoise(s) {
			continue
		}
		rows = append([]string{s}, rows...)
	}
	if len(rows) == 0 {
		return ""
	}
	var b strings.Builder
	b.WriteString(styleDim.Render("输出") + "\n")
	for _, r := range rows {
		b.WriteString("  " + styleDim.Render(truncate(r, m.width-4)) + "\n")
	}
	return b.String()
}

func isProgressNoise(s string) bool {
	if strings.HasSuffix(s, "%") || strings.HasSuffix(s, "\r") {
		return true
	}
	if n, err := strconv.Atoi(s); err == nil && n >= 0 {
		return true // the bare slice counter par2j redraws with CR
	}
	return false
}

func (m *model) viewResult() string {
	if !m.finished && m.code == 0 {
		return ""
	}
	style := styleOK
	if !m.ok {
		style = styleBad
	}
	return fmt.Sprintf("%s %s\n", styleDim.Render("结果"), style.Render(m.verdict))
}

func (m *model) viewHelp() string {
	return styleDim.Render(truncate(
		"空格 标记 · a 全选 · n 取消标记 · c 创建 · v 校验 · r 修复 · l 列表 · b 分片 · R 冗余 · esc 取消 · q 退出",
		m.width-2))
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

// statusLabel renders par2j's status column in Chinese, leaving anything
// unexpected as it came.
func statusLabel(s string) string {
	switch strings.TrimSpace(s) {
	case "= Complete":
		return "完整"
	case "- Missing":
		return "丢失"
	case "Damaged":
		return "损坏"
	case "Repaired":
		return "已修复"
	case "Misnamed":
		return "改名"
	case "Good":
		return "正常"
	}
	return s
}

func humanSize(n int64) string {
	const unit = 1024
	if n < unit {
		return fmt.Sprintf("%d B", n)
	}
	units := []string{"KB", "MB", "GB", "TB"}
	v := float64(n)
	for _, u := range units {
		v /= unit
		if v < unit {
			return fmt.Sprintf("%.1f %s", v, u)
		}
	}
	return fmt.Sprintf("%.1f PB", v/unit)
}

func truncate(s string, max int) string {
	if max < 4 || len([]rune(s)) <= max {
		return s
	}
	r := []rune(s)
	return string(r[:max-1]) + "…"
}
