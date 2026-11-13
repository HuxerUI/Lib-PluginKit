import argparse
import json
import os
import select
import shutil
import signal
import subprocess
import sys
import tempfile
import time
from pathlib import Path


def command_line(command):
    return " ".join(f'"{arg}"' for arg in command) + "\n"


def run(host, profile, *args, commands=None):
    script = None
    if commands is not None:
        script = "".join(map(command_line, commands))
    process = subprocess.run(
        [str(host), str(profile), *map(str, args)],
        input=script,
        capture_output=True,
        encoding="utf-8",
        timeout=30,
    )
    replies = [json.loads(line) for line in process.stdout.splitlines()]
    if commands is not None:
        assert process.returncode == 0 and len(replies) == len(commands), process
        return replies
    assert len(replies) == 1 and (process.returncode == 0) == replies[0]["ok"], process
    return replies[0]


def check_error(reply, code):
    assert not reply["ok"] and reply["error"] == code, reply


def plugin(listing, plugin_id):
    return next(item for item in listing["plugins"] if item["id"] == plugin_id)


def verify_default_profile(host, scratch):
    local_data = scratch / "local app data"
    if sys.platform == "linux":
        env = dict(os.environ, XDG_DATA_HOME=str(local_data))
        profile = local_data / "huxerui" / "lib-pluginkit" / "preview"
    elif sys.platform == "darwin":
        env = dict(os.environ, HOME=str(local_data))
        profile = local_data / "Library" / "Application Support" / "HuxerUI" / "Lib-PluginKit" / "Preview"
    else:
        env = dict(os.environ, LOCALAPPDATA=str(local_data))
        profile = local_data / "HuxerUI" / "Lib-PluginKit" / "Preview"
    process = subprocess.run(
        [str(host)],
        input="list\nquit\n",
        capture_output=True,
        encoding="utf-8",
        env=env,
        timeout=30,
    )
    assert process.returncode == 0, process
    assert json.loads(process.stdout) == {"ok": True, "plugins": [], "importers": []}, process
    assert (profile / "profile.lock").is_file()


def verify_host_exit(host, profile, valid, hang_worker, fault_id, scratch):
    marker = scratch / "worker waiting"
    with subprocess.Popen(
        [str(host), str(profile)],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        encoding="utf-8",
        env=dict(os.environ, HPK_CSV_WAIT_MARKER=str(marker)),
    ) as process:
        def send(*command):
            process.stdin.write(command_line(command))
            process.stdin.flush()

        def reply():
            assert select.select([process.stdout], [], [], 5)[0], "Host did not reply"
            return json.loads(process.stdout.readline())

        pidfd = None
        queue = None
        try:
            send("import", valid, fault_id)
            assert reply()["ok"]
            send("list")
            pid = plugin(reply(), fault_id)["worker_pid"]
            assert pid is not None and os.getpgid(pid) == pid
            if sys.platform == "darwin":
                queue = select.kqueue()
                watch = select.kevent(
                    pid, filter=select.KQ_FILTER_PROC, flags=select.KQ_EV_ADD | select.KQ_EV_ONESHOT,
                    fflags=select.KQ_NOTE_EXIT,
                )
                queue.control([watch], 0, 0)

                def worker_exited(timeout):
                    events = queue.control(None, 1, timeout)
                    return any(event.fflags & select.KQ_NOTE_EXIT for event in events)
            else:
                exits = select.poll()
                pidfd = os.pidfd_open(pid)
                exits.register(pidfd, select.POLLIN)

                def worker_exited(timeout):
                    return bool(exits.poll(timeout * 1000))
            check_error(run(host, profile, "list"), "profile_busy")

            send("import", hang_worker, fault_id, 30000)
            deadline = time.monotonic() + 5
            while not marker.exists() and process.poll() is None and time.monotonic() < deadline:
                time.sleep(0.01)
            assert marker.is_file() and not worker_exited(0), "Worker did not reach the hanging import"
            process.kill()
            assert process.wait(timeout=5) == -signal.SIGKILL
            assert worker_exited(5), "Worker survived SIGKILL of the host"
        finally:
            if process.poll() is None:
                process.kill()
                process.wait(timeout=5)
            if pidfd is not None:
                try:
                    if not exits.poll(0):
                        signal.pidfd_send_signal(pidfd, signal.SIGKILL)
                except ProcessLookupError:
                    pass
                finally:
                    os.close(pidfd)
            if queue is not None:
                queue.close()
    assert run(host, profile, "import", valid, fault_id)["ok"]


def verify(host, package, faults, scratch):
    verify_default_profile(host, scratch)
    profile = scratch / "profile"
    valid = scratch / "quoted cells.csv"
    valid.write_bytes('\ufeffname,city,note\r\n"Chen, Lin",上海,"first\r\nsecond ""line"""\r\n'.encode("utf-8"))
    invalid = scratch / "invalid.csv"
    invalid.write_bytes(b"name,city\r\nChen\r\n")
    exit_worker = scratch / "exit.csv"
    exit_worker.write_bytes(b"exit")
    hang_worker = scratch / "wait.csv"
    hang_worker.write_bytes(b"wait")
    throw = scratch / "throw.csv"
    throw.write_bytes(b"throw")

    csv_id = "example.csv"
    fault_id = "example.csv-faults"
    module_name = {"linux": "plugin.so", "darwin": "plugin.dylib", "win32": "plugin.dll"}[sys.platform]
    digest = run(host, profile, "inspect", package)["digest"]
    fault_digest = run(host, profile, "inspect", faults)["digest"]
    check_error(run(host, profile, "install", package, "0" * 64), "approval_mismatch")
    wrong_contract = scratch / "wrong contract"
    wrong_contract.mkdir()
    shutil.copyfile(package / module_name, wrong_contract / module_name)
    manifest = json.loads((package / "plugin.json").read_text(encoding="utf-8"))
    manifest["contract"]["fingerprint"] = "0" * 64
    (wrong_contract / "plugin.json").write_text(json.dumps(manifest), encoding="utf-8")
    wrong_digest = run(host, profile, "inspect", wrong_contract)["digest"]
    check_error(run(host, profile, "install", wrong_contract, wrong_digest), "contract_mismatch")

    replies = run(host, profile, commands=[
        ("install", package, digest),
        ("install", faults, fault_digest),
        ("list",),
        ("import", valid, csv_id),
        ("list",),
        ("import", invalid, csv_id),
        ("import", exit_worker, fault_id),
        ("list",),
        ("import", valid, fault_id),
        ("import", hang_worker, fault_id, 200),
        ("list",),
        ("import", valid, fault_id),
        ("import", throw, fault_id),
        ("disable", csv_id),
        ("list",),
    ])
    assert replies[0]["ok"] and replies[1]["ok"], replies[:2]
    (
        discovered, imported, active, invalid_result, exited, after_exit,
        restarted, expired, after_timeout, recovered, failure, disabled, after_disable,
    ) = replies[2:]
    assert {item["plugin"] for item in discovered["importers"]} == {csv_id, fault_id}, discovered
    assert all(item["worker_pid"] is None for item in discovered["plugins"]), discovered
    expected = {"header": ["name", "city", "note"], "rows": [["Chen, Lin", "上海", 'first\r\nsecond "line"']]}
    assert imported["ok"] and imported["table"] == expected, imported
    assert invalid_result["ok"] and invalid_result["table"] is None, invalid_result
    assert invalid_result["issues"][0]["code"] == "csv.column_count", invalid_result
    csv_pid = plugin(active, csv_id)["worker_pid"]
    assert csv_pid is not None, active
    check_error(exited, "worker_exited")
    assert plugin(after_exit, fault_id)["worker_pid"] is None, after_exit
    assert plugin(after_exit, csv_id)["worker_pid"] == csv_pid, after_exit
    assert restarted["ok"] and restarted["table"]["header"] == ["value"], restarted
    check_error(expired, "deadline_exceeded")
    assert plugin(after_timeout, fault_id)["worker_pid"] is None, after_timeout
    assert plugin(after_timeout, csv_id)["worker_pid"] == csv_pid, after_timeout
    assert recovered["ok"] and recovered["table"] == restarted["table"], recovered
    check_error(failure, "plugin_failed")
    assert disabled["ok"] and plugin(after_disable, csv_id)["worker_pid"] is None, after_disable
    assert [item["plugin"] for item in after_disable["importers"]] == [fault_id], after_disable

    (profile / "profile.json.tmp").write_text("incomplete commit", encoding="utf-8")
    persisted = run(host, profile, "list")
    assert not plugin(persisted, csv_id)["enabled"] and plugin(persisted, fault_id)["enabled"], persisted
    assert all(item["worker_pid"] is None for item in persisted["plugins"]), persisted
    assert run(host, profile, "enable", csv_id)["ok"]
    assert run(host, profile, "import", valid, csv_id)["table"] == expected
    sample = Path(__file__).with_name("sample.csv").resolve()
    assert run(host, profile, "import", sample, csv_id)["table"] == {
        "header": ["name", "city"], "rows": [["Chen, Lin", "Shanghai"], ["Noah", "New York"]],
    }

    modified_profile = scratch / "modified profile"
    assert run(host, modified_profile, "install", package, digest)["ok"]
    installed = modified_profile / "packages" / digest / module_name
    with installed.open("ab") as module:
        module.write(b"changed")
    check_error(run(host, modified_profile, "import", valid, csv_id), "package_changed")
    assert run(host, modified_profile, "install", package, digest)["ok"]
    assert run(host, modified_profile, "import", valid, csv_id)["table"] == expected

    if sys.platform in ("linux", "darwin"):
        verify_host_exit(host, profile, valid, hang_worker, fault_id, scratch)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Verify the standalone CSV host and separately built plugins.")
    parser.add_argument("host", type=Path)
    parser.add_argument("package", type=Path)
    parser.add_argument("faults", type=Path)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="pluginkit csv ") as directory:
        verify(args.host.resolve(), args.package.resolve(), args.faults.resolve(), Path(directory))
    print("CSV example passed")
