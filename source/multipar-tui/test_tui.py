#!/usr/bin/env python3
"""End to end test of the MultiPar TUI.

The TUI needs a terminal, so this drives it through a pty: it answers the
capability queries a real terminal would answer, sends key presses, and reads
the frames back.  Assertions are made on the rendered text and on the files on
disk, so a regression in either the model or the par2j driver fails the test.

    TUI_BIN=./multipar-tui PAR2J_BIN=../par2j/par2j ./test_tui.py
"""
import fcntl
import hashlib
import os
import pty
import re
import select
import struct
import sys
import tempfile
import termios
import time

HERE = os.path.dirname(os.path.abspath(__file__))
# absolute: the child chdirs into the work directory before exec
BIN = os.path.abspath(os.environ.get("TUI_BIN") or os.path.join(HERE, "multipar-tui"))
# absolute for the same reason: the TUI runs par2j with the work directory as
# its working directory, so a relative path would be resolved from there
PAR2J = os.path.abspath(os.environ.get("PAR2J_BIN") or os.path.join(HERE, "..", "par2j", "par2j"))

FAILED = []
ANSI = re.compile(r"\x1b\[[0-9;?]*[a-zA-Z]|\x1b\][^\x07\x1b]*(\x07|\x1b\\)|\x1b[()][A-Z]")


def check(cond, what):
    print(("  ok   " if cond else "  FAIL ") + what)
    if not cond:
        FAILED.append(what)


def dump(tui, title):
    """TUI_DEBUG=1 prints the last frames of a session, which is the only way
    to see what a failing run actually showed."""
    if os.environ.get("TUI_DEBUG"):
        print("---- %s ----\n%s---- end ----" % (title, tui.text()[-2000:]))


def answer(fd, chunk):
    """Reply to the queries Bubble Tea sends, as a real terminal would."""
    for query, reply in (
        (b"\x1b]11;?", b"\x1b]11;rgb:1e1e/1e1e/1e1e\x1b\\"),
        (b"\x1b]10;?", b"\x1b]10;rgb:ffff/ffff/ffff\x1b\\"),
        (b"\x1b[6n", b"\x1b[1;1R"),
        (b"\x1b[c", b"\x1b[?62;22c"),
        (b"\x1b[>c", b"\x1b[>0;276;0c"),
    ):
        if query in chunk:
            os.write(fd, reply)


class Tui:
    def __init__(self, workdir, *flags):
        self.buf = bytearray()
        self.pid, self.fd = pty.fork()
        if self.pid == 0:
            os.chdir(workdir)
            os.environ["TERM"] = "xterm-256color"
            os.execv(BIN, [BIN, "--dir", workdir] + list(flags))
        fcntl.ioctl(self.fd, termios.TIOCSWINSZ, struct.pack("HHHH", 34, 110, 0, 0))
        self.pump(1.0)

    def pump(self, seconds):
        end = time.time() + seconds
        while time.time() < end:
            r, _, _ = select.select([self.fd], [], [], 0.1)
            if not r:
                continue
            try:
                data = os.read(self.fd, 65536)
            except OSError:
                return
            if not data:
                return
            self.buf.extend(data)
            answer(self.fd, data)

    def send(self, keys, wait):
        os.write(self.fd, keys.encode())
        self.pump(wait)

    def keys(self, script):
        """script: "a:0.5,c:10,q:1" -- key : seconds to wait afterwards."""
        for step in script.split(","):
            key, _, delay = step.partition(":")
            self.send(key, float(delay or 0.5))

    def text(self):
        return ANSI.sub("", self.buf.decode("utf-8", "replace")).replace("\r", "\n")

    def close(self):
        self.send("q", 0.8)
        try:
            os.close(self.fd)
        except OSError:
            pass
        os.waitpid(self.pid, 0)


def md5(path):
    h = hashlib.md5()
    with open(path, "rb") as fh:
        for block in iter(lambda: fh.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def main():
    if not os.access(BIN, os.X_OK):
        print("no TUI binary at %s (run: go build -o multipar-tui .)" % BIN)
        return 1
    if not os.access(PAR2J, os.X_OK):
        print("no par2j binary at %s" % PAR2J)
        return 1
    os.environ["PAR2J_BIN"] = PAR2J

    work = tempfile.mkdtemp(prefix="multipar-tui-")
    os.chdir(work)
    sizes = {"a.bin": 1_400_000, "b.bin": 1_400_000}
    for name, size in sizes.items():
        with open(name, "wb") as fh:
            fh.write(os.urandom(size))
    before = {name: md5(name) for name in sizes}
    print("work dir: %s" % work)

    print("== create from the TUI (mark all, then c)")
    tui = Tui(work, "--rr", "100")
    check("MultiPar TUI" in tui.text(), "the TUI renders")
    tui.keys("a:0.5,c:10")
    created = [f for f in os.listdir(work) if f.endswith(".par2")]
    check(bool(created), "created a recovery file: %s" % ", ".join(sorted(created)))
    check("创建成功" in tui.text(), "reports 创建成功")
    dump(tui, "create")
    tui.close()

    print("== verify a complete set")
    tui = Tui(work)
    tui.keys("v:6")
    check("正常结束" in tui.text(), "reports 正常结束 · 无需修复")
    tui.close()

    print("== verify with a missing file")
    victim = "a.bin"
    os.unlink(victim)
    tui = Tui(work)
    tui.keys("v:6")
    text = tui.text()
    check("可修复" in text, "reports 输入文件不完整 + 可修复")
    check("丢失" in text and victim in text, "lists %s as 丢失" % victim)
    tui.close()

    print("== repair from the TUI")
    tui = Tui(work)
    tui.keys("r:12")
    text = tui.text()
    check("修复成功" in text, "reports 修复成功")
    check("已修复" in text and victim in text, "lists %s as 已修复" % victim)
    tui.close()

    print("== files are back bit for bit")
    check(os.path.exists(victim), "%s exists again" % victim)
    if os.path.exists(victim):
        check(md5(victim) == before[victim], "%s matches its original md5" % victim)

    print("== verify after repair")
    tui = Tui(work)
    tui.keys("v:6")
    check("All Files Complete" in tui.text(), "verify reports All Files Complete")
    tui.close()

    if FAILED:
        print("\n%d TEST(S) FAILED" % len(FAILED))
        return 1
    print("\nALL TESTS PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
