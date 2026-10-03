#!/usr/bin/env python3
"""Exercise encoding and interactive CLI boundaries without transmitting packets."""
import errno
import os
from pathlib import Path
import pty
import select
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
BIN = str(ROOT / "broadcast_send")
BASE_ENV = dict(os.environ, LC_ALL="C", BCS_TERMINAL_ENCODING="UTF-8",
                BCS_FILENAME_ENCODING="UTF-8")
CONFIG = """[network]
interface = example0
source_ip = 192.0.2.10
source_port = 0
broadcast_ip = 192.0.2.255
destination_port = 50000
[payload]
format = hex
file = {payload}
[send]
period_us = 1000
packets_per_cycle = 1
duration_sec = 1
max_attempts = 0
[stats]
interval_sec = 1
"""


def run(*args, data=b"", overrides=None, expected=0):
    env = dict(BASE_ENV, **(overrides or {}))
    proc = subprocess.run([BIN, *args], input=data, stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE, env=env, cwd=ROOT, timeout=10)
    accepted = expected if isinstance(expected, tuple) else (expected,)
    assert proc.returncode in accepted, (args, proc.returncode, proc.stdout, proc.stderr)
    assert "送信開始".encode() not in proc.stdout
    return proc.stdout, proc.stderr


def test_encodings(tmp):
    payload = tmp / "日本語.hex"
    payload.write_bytes(b"\xef\xbb\xbf00 ff\r\n")
    for config_encoding, python_encoding in (("UTF-8", "utf-8-sig"),
                                              ("EUC-JP", "euc_jp"), ("SJIS", "cp932")):
        ini = tmp / "send.ini"
        content = "# 日本語のコメント\r\n" + CONFIG.format(payload=payload.name)
        ini.write_bytes(content.encode(python_encoding))
        for terminal, decoder in (("UTF-8", "utf-8"), ("EUC-JP", "euc_jp"), ("SJIS", "cp932")):
            out, err = run("-c", str(ini), "-n", "--config-encoding", config_encoding,
                           overrides={"BCS_TERMINAL_ENCODING": terminal})
            decoded = out.decode(decoder)
            assert "日本語.hex (hex, 2 bytes)" in decoded, decoded
            assert "設定とペイロードの静的検証: OK" in decoded
            assert not err
        # ASCII-only values with legacy comments work in automatic mode.
        (tmp / "p.hex").write_bytes(b"01")
        ini.write_bytes(content.replace(payload.name, "p.hex").encode(python_encoding))
        run("-c", str(ini), "-n")

    # Ambiguity must not silently select a different filename.
    ini.write_bytes(CONFIG.format(payload="あ.hex").encode("euc_jp"))
    _, err = run("-c", str(ini), "-n", expected=1)
    assert b"--config-encoding" in err
    (tmp / "あ.hex").write_bytes(b"00")
    run("-c", str(ini), "-n", "--config-encoding", "EUC-JP")

    # Filesystems can also contain legacy-encoded names, independently of the terminal.
    for encoding, codec in (("EUC-JP", "euc_jp"), ("SJIS", "cp932")):
        native = os.fsencode(tmp) + b"/" + "送信.hex".encode(codec)
        with open(native, "wb") as stream:
            stream.write(b"00")
        ini.write_text(CONFIG.format(payload="送信.hex"), encoding="utf-8")
        out, _ = run("-c", str(ini), "-n", overrides={"BCS_FILENAME_ENCODING": encoding})
        assert "送信.hex (hex, 1 bytes)" in out.decode()

    # Binary data, including text/BOM/NUL bytes, is never transcoded.
    binary = b"\xef\xbb\xbf" + "日本語".encode("cp932") + b"\0"
    (tmp / "raw.bin").write_bytes(binary)
    ini.write_text(CONFIG.format(payload="raw.bin").replace("format = hex", "format = binary"))
    out, _ = run("-c", str(ini), "-n")
    assert f"binary, {len(binary)} bytes".encode() in out
    _, err = run("-c", str(ini), "-n", overrides={"BCS_TERMINAL_ENCODING": "bogus"}, expected=1)
    assert b"BCS_TERMINAL_ENCODING" in err


def test_prompts(tmp):
    ini = tmp / "partial.ini"
    (tmp / "p.hex").write_bytes(b"00")
    partial = CONFIG.format(payload="p.hex").replace("period_us = 1000", "period_us =")
    ini.write_text(partial)
    out, err = run("-c", str(ini), "-n", "--interactive", data=b"99\n2000\n\n")
    assert b"period_us            : 2000" in out
    assert err.count(b"(period_us)") == 2
    assert b"(destination_port)" not in err
    assert b"(duration_sec)" not in err
    _, err = run("-c", str(ini), "-n", "--non-interactive", expected=1)
    assert "必須項目がありません" in err.decode()
    run("-c", str(ini), "-n", "--interactive", expected=1)  # EOF
    run("-c", str(tmp / "missing.ini"), "-n", "--non-interactive", expected=1)
    # --stats-file satisfies that setting before the wizard asks questions.
    out, err = run("-c", str(ini), "-n", "--interactive", "-s", str(tmp / "stats.csv"), data=b"1000\n")
    assert "統計CSVファイル" not in err.decode()
    assert not (tmp / "stats.csv").exists()
    # Input paths are relative to cwd, and are decoded using the terminal encoding.
    for terminal, codec in (("EUC-JP", "euc_jp"), ("SJIS", "cp932")):
        p = tmp / "入力.hex"
        p.write_bytes(b"02")
        ini.write_text(CONFIG.format(payload=""))
        out, err = run("-c", str(ini), "-n", "--interactive",
                       data=(str(p) + "\n\n").encode(codec),
                       overrides={"BCS_TERMINAL_ENCODING": terminal})
        assert "入力.hex (hex, 1 bytes)" in out.decode(codec)
        err.decode(codec)


def test_zero(tmp):
    ini = tmp / "zero.ini"
    base = CONFIG.format(payload="unused.hex")
    zero = base.replace("format = hex\nfile = unused.hex", "format = zero\nsize = 256")
    ini.write_text(zero)
    out, _ = run("-c", str(ini), "-n", "--non-interactive")
    assert b"(zero, 256 bytes)" in out
    assert b"planned_payload_Mbps : 2.048" in out
    # Interactive zero mode asks for a size, never a filename.
    ini.write_text(base.replace("format = hex\nfile = unused.hex", ""))
    out, err = run("-c", str(ini), "-n", "-i", data=b"zero\n0\n65508\n512\n\n")
    assert b"(zero, 512 bytes)" in out
    assert err.count(b"(size)") == 3
    assert "ペイロードファイル (" not in err.decode()
    out, _ = run("-c", str(ini), "-n", "-i", data=b"\n\n\n")
    assert b"(zero, 100 bytes)" in out
    # The format can also be preconfigured; only the missing size is asked.
    ini.write_text(zero.replace("size = 256", "size ="))
    out, err = run("-c", str(ini), "-n", "-i", data=b"1024\n\n")
    assert b"(zero, 1024 bytes)" in out
    assert b"(format)" not in err
    run("-c", str(ini), "-n", "-i", expected=1)  # EOF at size prompt
    _, err = run("-c", str(ini), "-n", "--non-interactive", expected=1)
    assert b"[payload] size" in err
    # A previous filename is ignored, including one that does not exist.
    ini.write_text(zero.replace("size = 256", "size = 65507\nfile = /no/such/payload"))
    out, _ = run("-c", str(ini), "-n")
    assert b"(zero, 65507 bytes)" in out
    for size in ("0", "65508", "-1", "1.5"):
        ini.write_text(zero.replace("size = 256", "size = " + size))
        _, err = run("-c", str(ini), "-n", expected=1)
        assert b"size:" in err


def test_tty(tmp):
    ini = tmp / "tty.ini"
    ini.write_text(CONFIG.format(payload="p.hex").replace("period_us = 1000", "period_us ="))
    master, slave = pty.openpty()
    proc = subprocess.Popen([BIN, "-c", str(ini), "-n"], stdin=slave, stdout=slave,
                            stderr=slave, env=BASE_ENV, cwd=ROOT)
    os.close(slave)
    captured = b""
    answered = False
    deadline = time.monotonic() + 10
    try:
        while time.monotonic() < deadline:
            ready, _, _ = select.select([master], [], [], 0.1)
            if ready:
                try:
                    chunk = os.read(master, 65536)
                except OSError as exc:
                    if exc.errno == errno.EIO:
                        break
                    raise
                if not chunk:
                    break
                captured += chunk
            if not answered and b"(period_us)" in captured:
                os.write(master, b"3000\n\n")
                answered = True
        proc.wait(timeout=1)
        assert proc.returncode == 0, captured
        assert answered and b"period_us            : 3000" in captured, captured
    finally:
        if proc.poll() is None:
            proc.kill()
            proc.wait()
        os.close(master)


def test_discovery(tmp):
    # This calls getifaddrs, but --check-config never creates a sending socket.
    answers = b"1\n" + b"\n" * 10
    out, err = run("--interactive", "--check-config", data=answers, expected=(0, 1))
    if "設定に合う送信可能なNIC / IPv4がありません" in err.decode():
        print("test_usability: live discovery skipped (no eligible broadcast NIC)")
        return
    assert b"IPv4=" in err and b"broadcast=" in err
    assert "自動補完" in err.decode()
    assert "静的検証: OK" in out.decode()
    assert b"(zero, 100 bytes)" in out
    # Missing configuration files start the same wizard in interactive mode.
    out, err = run("-c", str(tmp / "missing.ini"), "-i", "-n", data=answers)
    assert "設定ファイルがありません" in err.decode()
    assert not (tmp / "missing.ini").exists()


with tempfile.TemporaryDirectory(prefix="bcs_usability_") as directory:
    folder = Path(directory)
    test_encodings(folder)
    test_prompts(folder)
    test_zero(folder)
    test_tty(folder)
    test_discovery(folder)
print("test_usability: OK")
