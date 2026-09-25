// Package main -- back end of the MultiPar TUI.
//
// The whole front end is a driver for the native par2j binary: it never links
// the repair code, it runs the same executable the regression suite covers, so
// there is exactly one implementation of the PAR2 format in play.  Two channels
// carry the state of a run:
//
//   - stdout is the human readable report, parsed for the header (sizes, slice
//     counts) and the per file status table;
//   - stderr carries one JSON object per line when PAR2J_PROGRESS is set, which
//     is the machine readable progress stream.
//
// Exit codes are a bit mask, documented in Command_par2j.txt; DescribeExit
// turns them back into something a caller can branch on.
package main

import (
	"bufio"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"sort"
	"strconv"
	"strings"
)

// Op is a par2j sub command.
type Op string

const (
	OpCreate Op = "c"
	OpVerify Op = "v"
	OpRepair Op = "r"
	OpList   Op = "l"
)

// Job is one par2j invocation.
type Job struct {
	Op         Op
	ParFile    string // recovery file to create or to read
	Inputs     []string
	BaseDir    string // -d: where the input files live
	BlockSize  int64  // -ss, 0 = let par2j choose
	Redundancy int    // -rr percent, create only
}

func (j Job) Args() []string {
	args := []string{string(j.Op)}
	if j.BlockSize > 0 {
		args = append(args, fmt.Sprintf("-ss%d", j.BlockSize))
	}
	if j.Redundancy > 0 && j.Op == OpCreate {
		args = append(args, fmt.Sprintf("-rr%d", j.Redundancy))
	}
	if j.BaseDir != "" {
		args = append(args, "-d"+j.BaseDir)
	}
	args = append(args, j.ParFile)
	for _, in := range j.Inputs {
		args = append(args, trimDirSlash(in))
	}
	return args
}

// trimDirSlash drops trailing separators from an input argument.  par2j reads a
// name ending in a separator as "record this empty folder, do not look inside"
// (the first branch of search_files() in par2_cmd.c), which is the opposite of
// what a trailing slash means when it came from shell completion or from
// --inputs /data/dir/.  A bare "/" is kept so the file system root stays
// addressable.
func trimDirSlash(p string) string {
	for len(p) > 1 && strings.HasSuffix(p, "/") {
		p = p[:len(p)-1]
	}
	return p
}

// Event is one progress line from the stderr stream.
type Event struct {
	Promille *int    `json:"promille"`
	Count    *int    `json:"count"`
	Phase    string  `json:"phase"`
	File     *string `json:"file"`
	Done     bool    `json:"done"`
}

// FileStatus is one row of the report table.
type FileStatus struct {
	Status string
	Name   string
}

// Snapshot is what the front end knows about the run so far, accumulated from
// the human readable output.
type Snapshot struct {
	Stage         string
	TotalSize     int64
	SliceCount    int
	SliceSize     int64
	RecoveryCount int
	RecoveryFound int
	SourceFiles   []FileStatus
	Statuses      []FileStatus
}

var (
	reStatus = regexp.MustCompile(`^\s*([^:"]+?)\s*:\s*"(.*)"\s*$`)
	reStage  = regexp.MustCompile(`^([A-Za-z][^:]*?)\s*:\s*$`)
)

// Feed folds one stdout line into the snapshot.
func (s *Snapshot) Feed(line string) {
	switch line = strings.TrimRight(line, " \t"); {
	case strings.HasPrefix(line, "Input File total size"):
		s.TotalSize = trailingInt64(line)
	case strings.HasPrefix(line, "Input File Slice count"):
		s.SliceCount = int(trailingInt64(line))
	case strings.HasPrefix(line, "Input File Slice size"):
		s.SliceSize = trailingInt64(line)
	case strings.HasPrefix(line, "Recovery Slice count"):
		s.RecoveryCount = int(trailingInt64(line))
	case strings.HasPrefix(line, "Recovery Slice found"):
		s.RecoveryFound = int(trailingInt64(line))
	}
	if m := reStatus.FindStringSubmatch(line); m != nil {
		row := FileStatus{Status: strings.TrimSpace(m[1]), Name: m[2]}
		switch {
		case strings.Contains(row.Status, "Slice"):
			// "Size Status : Filename" style rows carry the count, not a file
		case s.Stage == "Input File list":
			s.SourceFiles = append(s.SourceFiles, row)
		default:
			s.Statuses = append(s.Statuses, row)
		}
		return
	}
	if m := reStage.FindStringSubmatch(line); m != nil {
		s.Stage = strings.TrimSpace(m[1])
	}
}

func trailingInt64(line string) int64 {
	fields := strings.Fields(line)
	if len(fields) == 0 {
		return 0
	}
	v, _ := strconv.ParseInt(fields[len(fields)-1], 10, 64)
	return v
}

// DescribeExit turns the bit mask documented in Command_par2j.txt into a
// sentence plus a verdict.  ok is false whenever the user should look at it.
func DescribeExit(code int) (string, bool) {
	switch code {
	case 0:
		return "正常结束 · 无需修复", true
	case 1:
		return "致命错误", false
	case 2:
		return "被用户取消", false
	case 16:
		return "修复成功", true
	}
	var parts []string
	if code&1 != 0 {
		parts = append(parts, "致命错误")
	}
	if code&2 != 0 {
		parts = append(parts, "被取消")
	}
	if code&4 != 0 {
		parts = append(parts, "输入文件不完整")
	}
	if code&8 != 0 {
		parts = append(parts, "恢复块不足")
	}
	if code&16 != 0 {
		parts = append(parts, "修复失败")
	}
	if code&32 != 0 {
		parts = append(parts, "可改名/移动/还原")
	}
	if code&64 != 0 {
		parts = append(parts, "可重组/重建")
	}
	if code&128 != 0 {
		parts = append(parts, "可修复")
	}
	if code&256 != 0 {
		parts = append(parts, "PAR 文件不完整")
	}
	if len(parts) == 0 {
		return fmt.Sprintf("未知退出码 %d", code), false
	}
	ok := code&(1|2|8|16) == 0 && code&4 != 0 // damaged but nothing failed
	return strings.Join(parts, " + "), ok
}

// Entry is one file in the working directory.
type Entry struct {
	Name  string
	Size  int64
	IsPar bool
}

func ListDir(dir string) ([]Entry, error) {
	des, err := os.ReadDir(dir)
	if err != nil {
		return nil, err
	}
	out := make([]Entry, 0, len(des))
	for _, de := range des {
		if de.IsDir() {
			continue
		}
		info, err := de.Info()
		if err != nil {
			continue
		}
		out = append(out, Entry{Name: de.Name(), Size: info.Size(), IsPar: IsParName(de.Name())})
	}
	sort.Slice(out, func(i, j int) bool {
		if out[i].IsPar != out[j].IsPar {
			return !out[i].IsPar
		}
		return out[i].Name < out[j].Name
	})
	return out, nil
}

func IsParName(name string) bool {
	l := strings.ToLower(name)
	return strings.HasSuffix(l, ".par2") || strings.HasSuffix(l, ".par")
}

// RunHandle is a started par2j.
type RunHandle struct {
	Cmd    *exec.Cmd
	Cancel context.CancelFunc
}

// StartJob runs one par2j and streams its output through the callbacks.  onLine
// sees stdout lines (split on CR and LF, because par2j redraws progress with
// CR), onEvent sees the JSON progress stream, onDone fires once with the exit
// code.
func StartJob(ctx context.Context, bin, dir string, job Job, tickMS int,
	onLine func(string), onEvent func(Event), onDone func(int, error)) (*RunHandle, error) {

	ctx, cancel := context.WithCancel(ctx)
	cmd := exec.CommandContext(ctx, bin, job.Args()...)
	cmd.Dir = dir
	if tickMS <= 0 {
		tickMS = 100
	}
	cmd.Env = append(os.Environ(),
		"PAR2J_PROGRESS=1",
		fmt.Sprintf("PAR2J_PROGRESS_INTERVAL=%d", tickMS))

	stdout, err := cmd.StdoutPipe()
	if err != nil {
		cancel()
		return nil, err
	}
	stderr, err := cmd.StderrPipe()
	if err != nil {
		cancel()
		return nil, err
	}
	if err := cmd.Start(); err != nil {
		cancel()
		return nil, err
	}

	go func() {
		scanMixed(stdout, onLine)
	}()
	go func() {
		sc := bufio.NewScanner(stderr)
		sc.Buffer(make([]byte, 0, 64*1024), 1<<20)
		for sc.Scan() {
			var ev Event
			if json.Unmarshal(sc.Bytes(), &ev) == nil {
				onEvent(ev)
			}
		}
	}()
	go func() {
		err := cmd.Wait()
		code := 0
		var ee *exec.ExitError
		if err != nil {
			if errors.As(err, &ee) {
				code = ee.ExitCode()
				err = nil
			}
		}
		onDone(code, err)
	}()
	return &RunHandle{Cmd: cmd, Cancel: cancel}, nil
}

// scanMixed emits text chunks separated by CR or LF.
func scanMixed(r io.Reader, emit func(string)) {
	br := bufio.NewReaderSize(r, 64*1024)
	var sb strings.Builder
	for {
		b, err := br.ReadByte()
		if err != nil {
			break
		}
		if b == '\r' || b == '\n' {
			if sb.Len() > 0 {
				emit(sb.String())
				sb.Reset()
			}
			continue
		}
		sb.WriteByte(b)
		if sb.Len() >= 8192 {
			emit(sb.String())
			sb.Reset()
		}
	}
	if sb.Len() > 0 {
		emit(sb.String())
	}
}

// ResolveBin finds the par2j binary: explicit flag, $PAR2J_BIN, next to this
// binary, next to the source tree, then PATH.
func ResolveBin(flag string) string {
	// an explicit path is made absolute here: par2j is started with the work
	// directory as its working directory, so a relative path given from the
	// caller's directory would not resolve there
	if flag != "" {
		return absOrSelf(flag)
	}
	if v := os.Getenv("PAR2J_BIN"); v != "" {
		return absOrSelf(v)
	}
	// next to this binary: <dir>/par2j and <dir>/../par2j/par2j (source tree)
	if self, err := os.Executable(); err == nil {
		d := filepath.Dir(self)
		for _, cand := range []string{
			filepath.Join(d, "par2j"),
			filepath.Join(d, "..", "par2j", "par2j"),
		} {
			if isExecutable(cand) {
				return cand
			}
		}
	}
	// relative to the working directory, which is how `go run` is used here
	for _, cand := range []string{"par2j", filepath.Join("..", "par2j", "par2j")} {
		if p, err := filepath.Abs(cand); err == nil && isExecutable(p) {
			return p
		}
	}
	if p, err := exec.LookPath("par2j"); err == nil {
		return p
	}
	return "par2j"
}

func absOrSelf(path string) string {
	if a, err := filepath.Abs(path); err == nil {
		return a
	}
	return path
}

func isExecutable(path string) bool {
	st, err := os.Stat(path)
	return err == nil && !st.IsDir() && st.Mode()&0o111 != 0
}
