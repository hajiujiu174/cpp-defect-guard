"""Persist benchmark command output and state before a command has completed."""
import json
from pathlib import Path
import subprocess
import time


def run_logged(label, command, output, commands, env=None, cwd=None, timeout=300, exits=(0,)):
    output = Path(output)
    argv = [str(value) for value in command]
    record = dict(label=label, command=argv, state='starting', allowed_exit_codes=list(exits))
    commands.append(record)

    def checkpoint():
        pending = output / 'commands.json.tmp'
        pending.write_text(json.dumps(commands, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
        pending.replace(output / 'commands.json')

    stdout = output / (label + '.stdout.log')
    stderr = output / (label + '.stderr.log')
    start = time.monotonic()
    process = None
    checkpoint()
    try:
        with stdout.open('wb') as out_stream, stderr.open('wb') as err_stream:
            process = subprocess.Popen(argv, cwd=cwd or output, env=env, stdout=out_stream, stderr=err_stream)
            record.update(state='running', pid=process.pid)
            checkpoint()
            try:
                code = process.wait(timeout=timeout)
            except BaseException:
                # Preserve subprocess.run's direct-child cleanup semantics.
                if process.poll() is None:
                    process.kill()
                process.wait()
                raise
            record.update(state='completed' if code in exits else 'failed', exit_code=code)
        if code not in exits:
            raise RuntimeError(f'{label}: unexpected exit {code}; inspect log')
        return stdout.read_bytes().decode('utf-8')
    except BaseException as error:
        if process is not None and process.poll() is None:
            process.kill()
            process.wait()
        record.update(state='timed_out' if isinstance(error, subprocess.TimeoutExpired) else 'failed',
                      error_type=type(error).__name__, error=str(error))
        if process is not None:
            record['exit_code'] = process.poll()
        raise
    finally:
        record['seconds'] = time.monotonic() - start
        checkpoint()
        # Raw files already exist during execution, including abrupt parent loss.
        (output / (label + '.log')).write_bytes(
            (stdout.read_bytes() if stdout.exists() else b'') + b'\n--- stderr ---\n' +
            (stderr.read_bytes() if stderr.exists() else b''))
