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


def verify_plugin_context(host, faults, fault_id, fault_digest, valid, scratch):
    profile = scratch / "插件 profile"
    data = profile / "data" / f"plugin-{fault_id}.data"
    log = data / "lifecycle.log"
    assert run(host, profile, "install", faults, fault_digest)["ok"]
    data.mkdir(parents=True)
    fail_activation = data / "fail-activation"
    fail_activation.touch()
    failed, inactive = run(host, profile, commands=[("import", valid, fault_id), ("list",)])
    check_error(failed, "plugin_failed")
    assert plugin(inactive, fault_id)["worker_pid"] is None and not log.exists(), inactive
    fail_activation.unlink()

    contexts, inactive = run(host, profile, commands=[
        ("check-plugin-context", fault_id, "disable"),
        ("list",),
    ])
    expected = [fault_id, "0.1.0", fault_digest, (profile / "packages" / fault_digest).as_posix(), data.as_posix()]
    assert contexts["ok"] and contexts["first"] == contexts["second"] == expected + ["2", "2"], contexts
    assert contexts["after_close"] == expected + ["2", "1"], contexts
    disabled = plugin(inactive, fault_id)
    assert not disabled["enabled"] and disabled["worker_pid"] is None, inactive
    assert log.read_text().splitlines() == ["activate", "deactivate"]

    enabled, contexts = run(host, profile, commands=[("enable", fault_id), ("check-plugin-context", fault_id)])
    assert enabled["ok"] and contexts["first"] == expected + ["2", "2"], contexts
    assert log.read_text().splitlines() == ["activate", "deactivate"] * 2


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
    service = scratch / "status.csv"
    service.write_bytes(b"status")
    reenter = scratch / "reenter.csv"
    reenter.write_bytes(b"reenter")
    background = scratch / "background.csv"
    background.write_bytes(b"background")
    other_task = scratch / "other-task.csv"
    other_task.write_bytes(b"other-task")
    service_wait = scratch / "status-wait.csv"
    service_wait.write_bytes(b"status-wait")
    sample = Path(__file__).with_name("sample.csv").resolve()

    csv_id = "example.csv"
    fault_id = "example.csv-faults"
    module_name = {"linux": "plugin.so", "darwin": "plugin.dylib", "win32": "plugin.dll"}[sys.platform]
    digest = run(host, profile, "inspect", package)["digest"]
    fault_digest = run(host, profile, "inspect", faults)["digest"]
    verify_plugin_context(host, faults, fault_id, fault_digest, valid, scratch)
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
        ("provide-status", digest),
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
    assert all(reply["ok"] for reply in replies[:3]), replies[:3]
    (
        discovered, imported, active, invalid_result, exited, after_exit,
        restarted, expired, after_timeout, recovered, failure, disabled, after_disable,
    ) = replies[3:]
    assert {item["plugin"] for item in discovered["importers"]} == {csv_id, fault_id}, discovered
    assert all(item["worker_pid"] is None for item in discovered["plugins"]), discovered
    expected = {"header": ["name", "city", "note"], "rows": [["Chen, Lin", "上海", 'first\r\nsecond "line"']]}
    assert imported["ok"] and imported["table"] == expected, imported
    assert imported["progress_rows"] == 1, imported
    assert invalid_result["ok"] and invalid_result["table"] is None, invalid_result
    assert invalid_result["progress_rows"] is None, invalid_result
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
    registered, reopened = run(host, profile, commands=[("provide-status", digest), ("import", valid, csv_id)])
    assert registered["ok"] and reopened["table"] == expected
    replies = run(host, profile, commands=[
        ("provide-status", digest),
        ("import", sample, csv_id),
        ("status",),
        ("list",),
        ("import", service, fault_id),
        ("status",),
        ("provide-reentry-status", fault_digest),
        ("import", service, fault_id),
        ("disable", fault_id),
        ("enable", fault_id),
        ("import", reenter, fault_id),
        ("import", background, fault_id),
        ("import", other_task, fault_id),
        ("status",),
        ("import", service_wait, fault_id, 1000),
        ("status",),
        ("list",),
        ("import", service, fault_id),
        ("status",),
    ])
    (
        registered, imported, status, active, denied, denied_status, granted, still_denied, disabled, enabled,
        reentered, unavailable, other_unavailable, before_timeout, expired, after_timeout, after_workers, restarted, after_restart,
    ) = replies
    assert registered["ok"] and imported["table"] == {
        "header": ["name", "city"], "rows": [["Chen, Lin", "Shanghai"], ["Noah", "New York"]],
    }
    assert status["last_imported_rows"] == 2, status
    assert imported["progress_rows"] == 2, imported
    check_error(denied, "service_denied")
    assert denied_status["last_imported_rows"] == 2, denied_status
    assert granted["ok"] and disabled["ok"] and enabled["ok"]
    check_error(still_denied, "service_denied")
    check_error(reentered, "reentrant_call")
    check_error(unavailable, "service_unavailable")
    check_error(other_unavailable, "service_unavailable")
    assert before_timeout["last_imported_rows"] == 2, before_timeout
    check_error(expired, "deadline_exceeded")
    assert after_timeout["last_imported_rows"] == 42, after_timeout
    assert plugin(after_workers, fault_id)["worker_pid"] is None, after_workers
    assert plugin(after_workers, csv_id)["worker_pid"] == plugin(active, csv_id)["worker_pid"], after_workers
    lifetime, after_cancel, recovered = run(host, profile, commands=[
        ("check-receiver-lifetime", fault_id),
        ("list",),
        ("import", valid, fault_id),
    ])
    assert lifetime["ok"], lifetime
    catalog = lifetime.pop("catalog_during_callback")
    assert plugin(catalog, fault_id)["worker_pid"] is not None, catalog
    assert any(item["plugin"] == fault_id for item in catalog["importers"]), catalog
    assert lifetime == {
        "ok": True, "first_rows": 1, "active_rows": 1, "second_called": False,
        "cancelled_delivered": False, "cancelled_released": True, "late_rows": 0,
    }, lifetime
    assert plugin(after_cancel, fault_id)["worker_pid"] is None, after_cancel
    assert recovered["ok"], recovered
    assert restarted["ok"] and after_restart["last_imported_rows"] == 41, after_restart

    modified_profile = scratch / "modified profile"
    assert run(host, modified_profile, "install", package, digest)["ok"]
    installed = modified_profile / "packages" / digest / module_name
    with installed.open("ab") as module:
        module.write(b"changed")
    check_error(run(host, modified_profile, "import", valid, csv_id), "package_changed")
    assert run(host, modified_profile, "install", package, digest)["ok"]
    registered, repaired = run(
        host, modified_profile, commands=[("provide-status", digest), ("import", valid, csv_id)],
    )
    assert registered["ok"] and repaired["table"] == expected

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
