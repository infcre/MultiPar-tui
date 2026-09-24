// Command multipar-tui is a terminal front end for the native par2j binary.
//
//	multipar-tui                 # interactive TUI in the current directory
//	multipar-tui --dir /data/x   # interactive TUI elsewhere
//	multipar-tui --plain --op v --par p.par2   # headless, prints the event stream
//
// The TUI drives par2j as a subprocess and reads its JSON progress stream, so
// the verified binary stays the only implementation of the format.
package main

import (
	"context"
	"flag"
	"fmt"
	"os"
	"os/signal"
	"strings"
	"syscall"

	tea "github.com/charmbracelet/bubbletea"
)

func main() {
	var (
		bin        = flag.String("bin", "", "par2j 可执行文件路径（默认 $PAR2J_BIN 或自动查找）")
		dir        = flag.String("dir", ".", "工作目录（源文件与 par2 所在处）")
		plain      = flag.Bool("plain", false, "不进入 TUI，直接运行一次并打印进度流")
		op         = flag.String("op", "v", "plain 模式下的命令：c/v/r/l")
		par        = flag.String("par", "", "plain 模式下的 par2 文件名")
		inputs     = flag.String("inputs", "", "plain 模式下创建时的输入文件，逗号分隔")
		blockSize  = flag.Int64("ss", 0, "分片大小（字节），0 = 让 par2j 决定")
		redundancy = flag.Int("rr", 10, "冗余百分比（创建时）")
	)
	flag.Parse()

	resolved := ResolveBin(*bin)
	if *plain {
		if err := runPlain(resolved, *dir, Op(*op), *par, *inputs, *blockSize, *redundancy); err != nil {
			fmt.Fprintln(os.Stderr, "错误:", err)
			os.Exit(1)
		}
		return
	}

	m := newModel(resolved, *dir)
	if *blockSize > 0 {
		m.blockSize = *blockSize
	}
	if *redundancy > 0 {
		m.redundancy = *redundancy
	}
	p := tea.NewProgram(m, tea.WithAltScreen())
	if _, err := p.Run(); err != nil {
		fmt.Fprintln(os.Stderr, "错误:", err)
		os.Exit(1)
	}
}

func runPlain(bin, dir string, op Op, par, inputs string, blockSize int64, redundancy int) error {
	job := Job{
		Op:         op,
		ParFile:    par,
		BlockSize:  blockSize,
		Redundancy: redundancy,
	}
	if inputs != "" {
		job.Inputs = strings.Split(inputs, ",")
	}
	if job.ParFile == "" {
		return fmt.Errorf("plain 模式需要 --par")
	}

	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()

	fmt.Printf("$ par2j %s\n", strings.Join(job.Args(), " "))
	done := make(chan struct{})
	code := 0
	_, err := StartJob(ctx, bin, dir, job, 100,
		func(line string) { fmt.Println("  | " + line) },
		func(ev Event) {
			switch {
			case ev.Done:
				fmt.Printf("  · 阶段结束 %s\n", ev.Phase)
			case ev.Promille != nil && *ev.Promille >= 0:
				fmt.Printf("  %5.1f%%  %s\n", float64(*ev.Promille)/10, ev.Phase)
			case ev.Count != nil && *ev.Count >= 0:
				fmt.Printf("  %5d 片  %s\n", *ev.Count, ev.Phase)
			}
		},
		func(c int, err error) {
			code = c
			if err != nil {
				fmt.Println("  进程错误:", err)
			}
			close(done)
		})
	if err != nil {
		return err
	}
	<-done
	verdict, _ := DescribeExit(code)
	fmt.Printf("exit %d · %s\n", code, verdict)
	return nil
}
