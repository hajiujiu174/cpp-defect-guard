from concurrent.futures import ThreadPoolExecutor
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
from real_repair_process import run_logged


class ProcessEvidenceTests(unittest.TestCase):
    def test_stdout_and_stderr_are_separate_and_exit_is_recorded(self):
        with tempfile.TemporaryDirectory() as directory:
            out = Path(directory)
            result = run_logged('ok', [sys.executable, '-c', 'import sys;print("result");print("diagnostic",file=sys.stderr)'], out, [])
            self.assertEqual(result.strip(), 'result')
            row = json.loads((out / 'commands.json').read_text(encoding='utf-8'))[0]
            self.assertEqual((row['state'], row['exit_code']), ('completed', 0))
            self.assertGreater(row['pid'], 0)
            self.assertIn(b'diagnostic', (out / 'ok.stderr.log').read_bytes())
            self.assertIn(b'result', (out / 'ok.log').read_bytes())

    def test_expected_negative_exit_is_accepted(self):
        with tempfile.TemporaryDirectory() as directory:
            out = Path(directory)
            run_logged('negative', [sys.executable, '-c', 'raise SystemExit(8)'], out, [], exits=(8,))
            self.assertEqual(json.loads((out / 'commands.json').read_text(encoding='utf-8'))[0]['state'], 'completed')

    def test_unexpected_exit_retains_diagnostics(self):
        with tempfile.TemporaryDirectory() as directory:
            out = Path(directory)
            with self.assertRaises(RuntimeError):
                run_logged('bad', [sys.executable, '-c', 'print("failure detail",flush=True);raise SystemExit(3)'], out, [])
            row = json.loads((out / 'commands.json').read_text(encoding='utf-8'))[0]
            self.assertEqual((row['state'], row['exit_code']), ('failed', 3))
            self.assertIn(b'failure detail', (out / 'bad.log').read_bytes())

    def test_spawn_failure_is_recorded(self):
        with tempfile.TemporaryDirectory() as directory:
            out = Path(directory)
            with self.assertRaises(OSError):
                run_logged('missing', [out / 'does-not-exist.exe'], out, [])
            self.assertEqual(json.loads((out / 'commands.json').read_text(encoding='utf-8'))[0]['state'], 'failed')

    def test_timeout_preserves_partial_output(self):
        with tempfile.TemporaryDirectory() as directory:
            out = Path(directory)
            with self.assertRaises(subprocess.TimeoutExpired):
                run_logged('timeout', [sys.executable, '-c', 'import time;print("before timeout",flush=True);time.sleep(30)'], out, [], timeout=1)
            row = json.loads((out / 'commands.json').read_text(encoding='utf-8'))[0]
            self.assertEqual(row['state'], 'timed_out')
            self.assertIsNotNone(row['exit_code'])
            self.assertIn(b'before timeout', (out / 'timeout.stdout.log').read_bytes())

    def test_running_state_and_output_are_visible_before_completion(self):
        with tempfile.TemporaryDirectory() as directory, ThreadPoolExecutor(max_workers=1) as executor:
            out = Path(directory)
            code = 'from pathlib import Path;import time;print("started",flush=True)\nwhile not Path("release").exists(): time.sleep(0.01)'
            future = executor.submit(run_logged, 'live', [sys.executable, '-c', code], out, [], timeout=15)
            try:
                deadline = time.monotonic() + 10
                while time.monotonic() < deadline:
                    raw = out / 'live.stdout.log'
                    if raw.exists() and b'started' in raw.read_bytes():
                        break
                    time.sleep(0.01)
                else:
                    self.fail('Child did not expose its partial output')
                row = json.loads((out / 'commands.json').read_text(encoding='utf-8'))[0]
                self.assertEqual(row['state'], 'running')
                self.assertFalse(future.done())
            finally:
                (out / 'release').touch()
                future.result(timeout=20)


if __name__ == '__main__':
    unittest.main()
