#!/usr/bin/env python3
import os
import pty
import select
import subprocess
import sys
import tempfile
import time

binary = os.path.abspath(sys.argv[1])
with tempfile.TemporaryDirectory(prefix='lamepaste-progress-') as tmp:
    master, slave = pty.openpty()
    proc = subprocess.Popen(
        [binary, '--no-clipboard', '--db', os.path.join(tmp, 'tty.db')],
        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=slave,
        close_fds=True,
    )
    os.close(slave)
    captured = bytearray()
    deadline = time.monotonic() + 2.0
    while time.monotonic() < deadline and b'Reading input' not in captured:
        ready, _, _ = select.select([master], [], [], 0.1)
        if ready:
            try:
                captured.extend(os.read(master, 4096))
            except OSError:
                break
    if b'Reading input' not in captured:
        proc.kill()
        raise SystemExit('FAIL: interactive terminal did not display input progress')
    proc.stdin.write(b'progress-test-payload' * 4096)
    proc.stdin.flush()
    time.sleep(0.35)
    ready, _, _ = select.select([master], [], [], 0.1)
    if ready:
        captured.extend(os.read(master, 4096))
    proc.stdin.close()
    while proc.poll() is None:
        ready, _, _ = select.select([master], [], [], 0.1)
        if ready:
            try:
                captured.extend(os.read(master, 4096))
            except OSError:
                break
    try:
        while True:
            captured.extend(os.read(master, 4096))
    except OSError:
        pass
    os.close(master)
    stdout = proc.stdout.read()
    if proc.returncode != 0:
        raise SystemExit(f'FAIL: progress-enabled capture exited {proc.returncode}')
    if stdout:
        raise SystemExit('FAIL: progress output contaminated stdout')
    if b'bytes' not in captured:
        raise SystemExit('FAIL: progress display did not report live input byte count')

    # Redirected stderr must remain free of terminal control sequences.
    data = os.path.join(tmp, 'input.bin')
    with open(data, 'wb') as f:
        f.write(b'redirected-progress-test')
    err = os.path.join(tmp, 'redirected.err')
    with open(data, 'rb') as src, open(err, 'wb') as dst:
        result = subprocess.run(
            [binary, '--no-clipboard', '--db', os.path.join(tmp, 'redirected.db')],
            stdin=src, stdout=subprocess.PIPE, stderr=dst, check=False,
        )
    if result.returncode != 0:
        raise SystemExit('FAIL: redirected capture failed')
    if b'\x1b' in open(err, 'rb').read():
        raise SystemExit('FAIL: ANSI progress leaked to redirected stderr')

print('PASS: interactive progress is live, stderr-only, and suppressed when redirected')
