#!/usr/bin/env python3
"""End-to-end test of the XABC Live firmware on a connected AI Passport.

Holds one serial connection for the whole run (opening the USB Serial/JTAG port
resets the chip), waits for Wi-Fi, optionally runs `nettest`, then for each round:
sends `talk`, delivers a spoken question, collects transcripts and the 2 s `stats`
lines, sends `hangup`, and prints a PASS/FAIL summary.

By default the test is silent: the question is synthesized with macOS `say`, served
from a temporary LAN HTTP server, and the device downloads it (`testmic on` +
`testplay <url>`) in place of the microphone, paced in real time; the device volume
is 0 for the round (playback is still measured). `--speaker` plays it aloud instead.

    python3 tools/live_autotest.py                       # one round
    python3 tools/live_autotest.py --rounds 3 --listen 25 --nettest

Requires pyserial (activated ESP-IDF Python) and macOS `say`. Raw logs are kept
locally (default /tmp/xabc_live_autotest.log) with API keys redacted.
"""

from __future__ import annotations

import argparse
import functools
import http.server
import socket
import threading
import audioop
import os
import tempfile
import wave
import re
import subprocess
import sys
import time

try:
    import serial
    from serial.tools import list_ports
except ImportError:  # pragma: no cover
    sys.exit("pyserial is required: activate ESP-IDF (. $IDF_PATH/export.sh)")

ANSI = re.compile(r"\x1b\[[0-9;]*m")
KEY = re.compile(r"sk-[A-Za-z0-9]{8,}")
STATS = re.compile(r"I \((\d+)\) app_live: stats .*?audio=(\d+)/\d+B .*?in_tx=(\d+) out_tx=(\d+) "
                   r".*?up=(\d+) fail=(\d+) drop=(\d+) .*?underrun=(\d+) spk_drop=(\d+) skip=(\d+) heap=(\d+)")


def find_port() -> str:
    ports = [p.device for p in list_ports.comports() if p.vid == 0x303A]
    if len(ports) != 1:
        sys.exit(f"Expected one Espressif USB device, found {ports or 'none'}; use --port.")
    return ports[0]


class Device:
    def __init__(self, port: str, log_path: str) -> None:
        self.ser = serial.Serial()
        self.ser.port, self.ser.baudrate, self.ser.timeout = port, 115200, 0.2
        self.ser.dtr = False
        self.ser.rts = False
        self.ser.open()
        self.buf = b""
        self.log = open(log_path, "w", encoding="utf-8")
        self.t0 = time.monotonic()

    def lines(self, seconds: float):
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            self.buf += self.ser.read(4096)
            while b"\n" in self.buf:
                raw, self.buf = self.buf.split(b"\n", 1)
                line = KEY.sub("sk-***", ANSI.sub("", raw.decode("utf-8", "replace"))).strip()
                if line:
                    self.log.write(f"{time.monotonic() - self.t0:8.2f} {line}\n")
                    yield line

    def wait_for(self, pattern: str, seconds: float, sink: list[str] | None = None) -> str | None:
        rx = re.compile(pattern)
        for line in self.lines(seconds):
            if sink is not None:
                sink.append(line)
            if rx.search(line):
                return line
        return None

    def send(self, cmd: str) -> None:
        self.ser.write(cmd.encode() + b"\n")

    def close(self) -> None:
        self.ser.close()
        self.log.close()


def analyse(lines: list[str]) -> dict:
    stats = [tuple(map(int, m.groups())) for m in (STATS.search(l) for l in lines) if m]
    you = "".join(l.split("app_live: you: ", 1)[1] for l in lines if "app_live: you: " in l)
    ai = "".join(l.split("app_live: ai: ", 1)[1] for l in lines if "app_live: ai: " in l)
    attempts = [l for l in lines if re.search(r"第 \d+ 次连接", l)]
    errors = [l for l in lines if l.startswith("E (") and "websocket_client: Client was not started" not in l]
    res = {"you": you, "ai": ai, "attempts": attempts, "errors": errors, "stats": stats}
    if len(stats) >= 2:
        (t0, a0, *_), last = stats[0], stats[-1]
        dt = (last[0] - t0) / 1000 or 1
        res.update(down_rate=(last[1] - a0) / dt, up_rate=(last[4] - stats[0][4]) / dt,
                   underrun=last[7] - stats[0][7], spk_drop=last[8], skip=last[9],
                   up_fail=last[5], up_drop=last[6], min_heap=min(s[10] for s in stats))
    return res


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port")
    ap.add_argument("--rounds", type=int, default=1)
    ap.add_argument("--listen", type=float, default=20, help="seconds to listen after speaking")
    ap.add_argument("--expect-close", action="store_true", help="the follow-up is a goodbye: the session must end")
    ap.add_argument("--followup", help="after the answer, ask this in the same session (e.g. 再讲一个)")
    ap.add_argument("--voice-barge", action="store_true",
                    help="with --barge: interrupt by voice only, without the OK key")
    ap.add_argument("--say", action="append",
                    help="question per round (repeat; cycles). Default: an introduction, then a joke")
    ap.add_argument("--voice", default="Tingting", help="macOS say voice")
    ap.add_argument("--nettest", action="store_true")
    ap.add_argument("--speaker", action="store_true", help="play the question aloud instead of injecting it")
    ap.add_argument("--barge", action="store_true",
                    help="interrupt test: ask for a long story, press OK while it talks, ask something new")
    ap.add_argument("--log", default="/tmp/xabc_live_autotest.log")
    args = ap.parse_args()

    dev = Device(args.port or find_port(), args.log)
    ok_all = True
    try:
        print("等待设备启动并联网…")
        boot_lines: list[str] = []
        if not dev.wait_for(r"已联网 .*IP=", 40, boot_lines):
            print("FAIL: 40 秒内没有联网")
            return 1
        dev.wait_for(r"$^", 2)   # let boot logs settle
        dev.send("debug on")       # stats + transcripts are off by default

        dev.wait_for(r"OK debug", 3)
        cfg: list[str] = []
        dev.send("config")
        dev.wait_for(r"OK config", 5, cfg)
        boot_lines += cfg
        volume = next((l.split("=", 1)[1] for l in cfg if l.startswith("volume=")), "70")
        rate = 8000
        autostart = any(l.strip() == "autostart=on" for l in cfg)
        base_url = ""
        serve_dir = ""
        if not args.speaker:
            serve_dir = tempfile.mkdtemp(prefix="xabc_live_")
            class Paced(http.server.SimpleHTTPRequestHandler):
                """Serves test audio at real-time pace so the device never buffers a burst
                (a LAN burst fills the 16 KB TCP window and starves TLS of RAM)."""
                def log_message(self, *a):
                    pass

                def copyfile(self, source, outputfile):
                    step = rate // 10                     # 100 ms of mu-law
                    while True:
                        chunk = source.read(step)
                        if not chunk:
                            break
                        outputfile.write(chunk)
                        outputfile.flush()
                        time.sleep(0.09)
            handler = functools.partial(Paced, directory=serve_dir)
            httpd = http.server.ThreadingHTTPServer(("0.0.0.0", 0), handler)
            threading.Thread(target=httpd.serve_forever, daemon=True).start()
            probe = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            probe.connect(("8.8.8.8", 53))             # no packet is sent: just picks the LAN address
            base_url = f"http://{probe.getsockname()[0]}:{httpd.server_address[1]}"
            probe.close()
            print(f"静默测试：8 kHz，语音经 {base_url} 注入，设备音量临时为 0")

        def make_audio(name: str, text: str) -> float:
            aiff, wav = os.path.join(serve_dir, name + ".aiff"), os.path.join(serve_dir, name + ".wav")
            subprocess.run(["say", "-v", args.voice, "-o", aiff, text], check=True)
            subprocess.run(["afconvert", "-f", "WAVE", "-d", f"LEI16@{rate}", "-c", "1", aiff, wav], check=True)
            with wave.open(wav) as w:
                data = audioop.lin2ulaw(w.readframes(w.getnframes()), 2)
            with open(os.path.join(serve_dir, name + ".ulaw"), "wb") as f:
                f.write(data)
            return len(data) / rate

        if args.nettest:
            out: list[str] = []
            dev.send("nettest")
            dev.wait_for(r"OK nettest", 120, out)
            for l in out:
                if "DNS" in l or l.startswith("Wi-Fi:"):
                    print("  " + l)
        questions = args.say or ["你好，请用两句话介绍一下你自己。", "给我讲一个简短的笑话。"]
        for r in range(1, args.rounds + 1):
            question = questions[(r - 1) % len(questions)]
            print(f"\n=== 第 {r} 轮：{question} ===")
            lines: list[str] = []
            if not args.speaker:
                dev.send("volume 0")
                dev.send("testmic on")
                dev.wait_for(r"OK testmic", 3, lines)
            t_start = time.monotonic()
            if r == 1 and autostart:
                # The firmware opens a conversation by itself after boot.
                started = next((l for l in boot_lines if "会话已开始" in l), None) or \
                    dev.wait_for(r"会话已开始|连接失败，按 OK|会话未开始|网络不通", 90, lines)
                print("开机自动进入对话：" + ("是" if started and "会话已开始" in started else "否"))
            else:
                dev.send("talk")
                started = dev.wait_for(r"会话已开始|连接失败，按 OK|会话未开始|Wi-Fi 未连接", 90, lines)
            if not started or "会话已开始" not in started:
                print(f"FAIL: 会话没有开始（{started or '超时'}）")
                for l in [l for l in lines if "次连接" in l]:
                    print("  " + l)
                ok_all = False
                continue
            print(f"会话开始，用时 {time.monotonic() - t_start:.1f} s")
            dev.wait_for(r"$^", 1.5, lines)
            if args.speaker:
                speaker = subprocess.Popen(["say", "-v", args.voice, question])
                while speaker.poll() is None:
                    dev.wait_for(r"$^", 0.5, lines)
            else:
                seconds = make_audio(f"q{r}", question)
                dev.send(f"testplay {base_url}/q{r}.ulaw")
                dev.wait_for(r"testplay done|testplay: cannot", seconds + 10, lines)
            if args.barge:
                # Wait until the assistant is talking, then interrupt it.
                if dev.wait_for(r"app_live: ai: ", 15, lines):
                    dev.wait_for(r"$^", 3, lines)
                    if not args.voice_barge:
                        dev.send("talk")                  # = OK click while speaking: interrupt
                    t_int = time.monotonic()
                    dev.wait_for(r"$^", 0.8, lines)
                    n_ai_before = sum("app_live: ai: " in l for l in lines)
                    second = "停一下，换个问题：一加一等于几？"
                    if args.speaker:
                        sp = subprocess.Popen(["say", "-v", args.voice, second])
                        while sp.poll() is None:
                            dev.wait_for(r"$^", 0.5, lines)
                    else:
                        secs = make_audio(f"b{r}", second)
                        dev.send(f"testplay {base_url}/b{r}.ulaw")
                        dev.wait_for(r"testplay done|testplay: cannot", secs + 10, lines)
                    dev.wait_for(r"$^", args.listen, lines)
                    after = [l.split("app_live: ai: ", 1)[1] for l in lines if "app_live: ai: " in l][n_ai_before:]
                    print(f"  打断后 {time.monotonic() - t_int:.1f} s 内 AI 新的回答：{''.join(after)[:80] or '（无）'}")
                    # "第二天" in a story must not count: require an actual answer to 1+1.
                    text = "".join(after)
                    answered = re.search(r"(等于|是|得)\s*[二2两]|(^|[。！？!?,，])\s*[二2两]\s*[。！!]|[二2两][。！!]?\s*$", text) is not None
                    print(f"  {'PASS' if answered else 'FAIL'}  打断后回答了新问题")
                    ok_all &= answered
                else:
                    print("  FAIL  AI 没有开始说话，无法测试打断")
                    ok_all = False
            else:
                dev.wait_for(r"$^", args.listen, lines)
                if args.followup and not args.speaker:
                    n_ai_before = sum("app_live: ai: " in l for l in lines)
                    secs = make_audio(f"f{r}", args.followup)
                    dev.send(f"testplay {base_url}/f{r}.ulaw")
                    dev.wait_for(r"testplay done|testplay: cannot", secs + 10, lines)
                    dev.wait_for(r"$^", args.listen, lines)
                    new_ai = [l.split("app_live: ai: ", 1)[1] for l in lines if "app_live: ai: " in l][n_ai_before:]
                    print(f"  追问「{args.followup}」→ {''.join(new_ai)[:120] or '（无回答）'}")
                    ok_all &= bool(new_ai)
                    closed = any("会话已释放" in l for l in lines)
                    print(f"  追问后对话{'已自动结束' if closed else '仍在进行'}")
                    if args.expect_close:
                        print(f"  {'PASS' if closed else 'FAIL'}  说告别后自动结束对话")
                        ok_all &= closed
            dev.send("hangup")
            dev.wait_for(r"会话已释放", 15, lines)
            if not args.speaker:
                dev.send("testmic off")
                dev.send(f"volume {volume}")
            res = analyse(lines)
            for l in res["attempts"]:
                print("  " + l.split("app_live: ", 1)[-1])
            print(f"  你说：{res['you'] or '（没有识别到）'}")
            print(f"  AI ：{res['ai'] or '（没有回答）'}")
            if "down_rate" in res:
                print(f"  下行 {res['down_rate']:.1f}/s  上行 {res['up_rate']:.1f}/s  卡顿 {res['underrun']} 次  "
                      f"跳过静音 {res['skip']} 帧  丢弃 {res['spk_drop']} B  上行失败 {res['up_fail']}  "
                      f"上行丢包 {res['up_drop']}  最低内存 {res['min_heap']} B")
            for l in res["errors"][:5]:
                print("  ERR " + l[:160])
            checks = {
                "识别到用户语音": bool(res["you"]),
                "收到 AI 回答": bool(res["ai"]),
                "下行接近实时 (≥9/s)": res.get("down_rate", 0) >= 9,
                "上行接近实时 (≥9/s)": res.get("up_rate", 0) >= 9,
                "卡顿 ≤ 2 次": res.get("underrun", 99) <= 2,
                "最低内存 ≥ 20 KB": res.get("min_heap", 0) >= 20000,
                "没有错误日志": not res["errors"],
            }
            for name, ok in checks.items():
                print(f"  {'PASS' if ok else 'FAIL'}  {name}")
            ok_all &= all(checks.values())
            dev.wait_for(r"$^", 3)
    finally:
        dev.close()
    print(f"\n总体：{'PASS' if ok_all else 'FAIL'}（原始日志：{args.log}）")
    return 0 if ok_all else 1


if __name__ == "__main__":
    sys.exit(main())
