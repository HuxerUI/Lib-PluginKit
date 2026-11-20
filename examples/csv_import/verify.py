import argparse
import csv
import hashlib
import json
import os
import select
import shutil
import signal
import subprocess
import sys
import tempfile
import time
from contextlib import contextmanager
from concurrent.futures import ThreadPoolExecutor
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


def verify_sdk(sdk):
    expected = {
        "csv_contract": "csv_import::TableImporter",
        "exporters/export_contract": "csv_import::TableExporter",
        "receivers/import_progress": "csv_import::ImportProgress",
        "services/import_status": "csv_import::ImportStatus",
    }
    assert {file.relative_to(sdk).as_posix() for file in sdk.rglob("*.contract.json")} == {
        f"{name}.contract.json" for name in expected
    }
    for name, interface in expected.items():
        [contract] = json.loads((sdk / f"{name}.contract.json").read_text(encoding="utf-8"))
        identity = ("cpp." + hashlib.sha256(interface.encode()).hexdigest(), 1)
        if interface == "csv_import::ImportStatus":
            identity = ("example.import-status", 2)
        assert contract["interface"] == interface and (contract["id"], contract["version"]) == identity, contract
        assert (sdk / f"{name}.bindings.h").is_file() and (sdk / f"{name}.host.h").is_file()
    assert (sdk / "csv_sdk.h").is_file()


def verify_sdk_republish(sdk, scratch):
    staged = scratch / "staged sdk"
    published = scratch / "published sdk"
    staged.mkdir()
    published.mkdir()
    filename = "import_progress.h"
    shutil.copyfile(sdk / "receivers" / filename, staged / filename)
    shutil.copyfile(staged / filename, published / "Import_Progress.h")
    files = staged / ".hpk-sdk-files"
    files.write_text(filename + "\n", encoding="utf-8")
    (published / files.name).write_text("Import_Progress.h\n", encoding="utf-8")
    subprocess.run(
        ["cmake", f"-D_HPK_SDK_DIRECTORY={published}", f"-D_HPK_SDK_SOURCE={staged}",
         f"-D_HPK_SDK_FILES={files}", "-P", str(sdk / "PluginKit.cmake")],
        check=True, timeout=30,
    )
    assert {path.name for path in published.iterdir()} == {filename, files.name}
    assert (published / filename).read_bytes() == (staged / filename).read_bytes()


def verify_pack(host, hpk, scratch):
    example = Path(__file__).resolve().parent
    project = scratch / "pack project"
    fixture = project / "fixture"
    platform = {"win32": "windows", "linux": "linux", "darwin": "macos"}[sys.platform]
    shutil.copytree(example / "plugin", fixture)
    shutil.copytree(example.parent / "preview" / "platform" / platform, project / "platform" / platform)
    sdk = (host.parents[3] if platform == "macos" else host.parent) / "sdk"
    verify_sdk(sdk)
    verify_sdk_republish(sdk, scratch)
    cmake = project / "CMakeLists.txt"

    def configure(fixtures):
        cmake.write_text(
            "cmake_minimum_required(VERSION 3.24)\n"
            "project(CsvPackCheck LANGUAGES CXX)\n"
            f'set(CSV_IMPORT_SDK "{sdk.as_posix()}" CACHE PATH "" FORCE)\n'
            f'set(CSV_IMPORT_TEST_FIXTURES {fixtures} CACHE BOOL "" FORCE)\n'
            "add_subdirectory(fixture)\n"
            'set_target_properties(csv-importer PROPERTIES OUTPUT_NAME csv-existing-target)\n'
            'file(GENERATE OUTPUT "csv-target-path.txt" CONTENT "$<TARGET_FILE:csv-importer>")\n',
            encoding="utf-8",
        )

    def pack(*options, error=None):
        abi = ["--abi", "x86_64"] if platform == "windows" else []
        process = subprocess.run(
            [str(hpk), "pack", platform, *abi, *options],
            cwd=fixture, capture_output=True, encoding="utf-8", errors="replace", timeout=180,
        )
        if error:
            assert process.returncode != 0 and error in process.stderr, process
            return process.stderr
        assert process.returncode == 0, process
        return json.loads(process.stdout.splitlines()[-1])

    configure("ON")
    ambiguity = pack(error="Select one with --target")
    assert all(name in ambiguity for name in ("csv-importer", "csv-importer-worker", "csv-faults")), ambiguity
    worker = pack("--target", "csv-importer-worker", "--config", "release")
    assert worker["target"] == "csv-importer-worker" and worker["id"] == "example.csv-worker", worker

    configure("OFF")
    package = pack()
    directory = Path(package["path"])
    sdk_key, variant, build_config, packages, target, config = directory.relative_to(project / ".huxerui" / "build").parts
    assert sdk_key.startswith("home-") and variant.startswith(platform) and build_config == "release", directory
    assert (packages, target, config) == ("packages", "csv-importer", "Release"), directory
    assert (directory / "resources" / "huxerui" / "resources.bin").is_file(), directory
    library = Path((directory.parents[2] / "csv-target-path.txt").read_text(encoding="utf-8"))
    module = {"windows": "plugin.dll", "linux": "plugin.so", "macos": "plugin.dylib"}[platform]
    assert "csv-existing-target" in library.name and library.read_bytes() == (directory / module).read_bytes(), library
    assert Path(worker["path"]).is_dir(), worker
    assert not list(directory.parent.glob("*.hpk-*")), directory
    pack("--target", "csv-importer-worker", error="This build produced no plugin package for target")

    profile = scratch / "pack profile"
    inspected = run(host, profile, "inspect", directory)
    assert inspected["ok"] and all(package[key] == inspected[key] for key in ("id", "version", "digest")), inspected
    assert run(host, profile, "install", directory, package["digest"], "worker_process", "status")["ok"]

    manifest = fixture / "plugin.json.in"
    source = manifest.read_text(encoding="utf-8")
    manifest.write_text(source.replace("strings/settings_max_rows", "strings/missing_pack_label"), encoding="utf-8")
    pack(error="Cannot resolve string resource")

    cmake.write_text(cmake.read_text(encoding="utf-8") + 'message(FATAL_ERROR "CSV build failed")\n', encoding="utf-8")
    pack(error="huxerui build failed with exit code")


def plugin(listing, plugin_id):
    return next(item for item in listing["plugins"] if item["id"] == plugin_id)


def installed_data(profile, plugin_id):
    state = json.loads((profile / "profile.json").read_text(encoding="utf-8"))
    record = next(entry for entry in state["plugins"] if entry["manifest"]["id"] == plugin_id)
    return profile / "data" / record["data"]


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
    assert json.loads(process.stdout) == {"ok": True, "plugins": [], "importers": [], "exporters": []}, process
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


def verify_plugin_context(host, faults, fault_id, fault_digest, valid, scratch, mode):
    profile = scratch / f"插件 profile {mode}"
    data = profile / "data" / f"plugin-{fault_id}.data"
    log = data / "lifecycle.log"
    assert run(host, profile, "install", faults, fault_digest, mode)["ok"]
    data.mkdir(parents=True, exist_ok=True)
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
    assert contexts["ok"] and contexts["first"] == contexts["second"] == expected + ["2", "2", "200000"], contexts
    assert contexts["after_close"] == expected + ["2", "1", "200000"], contexts
    disabled = plugin(inactive, fault_id)
    assert not disabled["enabled"] and disabled["worker_pid"] is None, inactive
    assert log.read_text().splitlines() == ["activate", "deactivate"]

    enabled, contexts = run(host, profile, commands=[("enable", fault_id), ("check-plugin-context", fault_id)])
    assert enabled["ok"] and contexts["first"] == expected + ["2", "2", "200000"], contexts
    assert log.read_text().splitlines() == ["activate", "deactivate"] * 2


def verify_mixed(host, package, worker_csv, faults, scratch, sample, valid):
    profile = scratch / "mixed profile"
    local_id = "example.csv"
    worker_id = "example.csv-worker"
    fault_id = "example.csv-faults"
    local_digest = run(host, profile, "inspect", package)["digest"]
    worker_digest = run(host, profile, "inspect", worker_csv)["digest"]
    fault_digest = run(host, profile, "inspect", faults)["digest"]
    big_csv = scratch / "large field.csv"
    big_csv.write_bytes(b"field\n" + b"x" * (8 * 1024 * 1024) + b"\n")
    installs = [
        ("install", package, local_digest, "in_process", "status"),
        ("install", worker_csv, worker_digest, "worker_process", "status"),
        ("install", faults, fault_digest, "worker_process"),
    ]
    assert all(reply["ok"] for reply in run(host, profile, commands=installs))
    data = installed_data(profile, fault_id)
    data.mkdir(parents=True)
    (data / "sync-service").touch()
    ssv = scratch / "quoted cells.ssv"
    ssv.write_bytes('name;city;note\r\n"Chen, Lin";上海;"first\r\nsecond ""line"""\r\n'.encode("utf-8"))
    replies = run(host, profile, commands=[
        ("pid",), ("list",),
        ("import", sample, local_id), ("status",), ("reset-status",),
        ("import", sample, worker_id), ("status",), ("list",),
        ("import", big_csv, local_id), ("import", big_csv, worker_id),
        ("import", sample, fault_id),
    ])
    host_pid, discovered, local, local_status, _, worker, worker_status, active, big_local, big_worker, sync_callback = replies
    assert {item["plugin"] for item in discovered["importers"]} == {local_id, worker_id, fault_id}, discovered
    assert all(item["worker_pid"] is None for item in discovered["plugins"]), discovered
    assert local["ok"] and worker["ok"], (local, worker)
    assert local["table"] == worker["table"] and local["delimiter"] == worker["delimiter"] == ","
    assert local["progress_rows"] == worker["progress_rows"] == 2
    assert local_status["last_imported_rows"] == worker_status["last_imported_rows"] == 2
    assert local["plugin_pid"] == host_pid["pid"] != worker["plugin_pid"]
    assert plugin(active, local_id)["mode"] == "in_process"
    assert plugin(active, worker_id)["mode"] == "worker_process"
    assert plugin(active, worker_id)["worker_pid"] == worker["plugin_pid"], active
    assert big_local["ok"] and len(big_local["table"]["rows"][0][0]) == 8 * 1024 * 1024
    check_error(big_worker, "payload_too_large")
    check_error(sync_callback, "callback_unavailable")

    for plugin_id, mode in ((local_id, "in_process"), (worker_id, "worker_process")):
        log = installed_data(profile, plugin_id) / "lifecycle.log"
        previous = log.read_text().splitlines()
        pid, round_trip, live, disabled, hidden, enabled, visible = run(host, profile, commands=[
            ("pid",), ("round-trip", valid, ssv, plugin_id), ("list",),
            ("disable", plugin_id), ("list",), ("enable", plugin_id), ("list",),
        ])
        assert round_trip["ok"] and round_trip["all_importers"] == 5 and round_trip["package_importers"] == 2, round_trip
        assert round_trip["format"] == "csv" and round_trip["csv"] == (
            '"name","city","note"\r\n"Chen, Lin","上海","first\r\nsecond ""line"""\r\n'
        ), round_trip
        if mode == "in_process":
            assert round_trip["plugin_pid"] == pid["pid"], round_trip
        else:
            assert round_trip["plugin_pid"] == plugin(live, plugin_id)["worker_pid"] != pid["pid"], live
        assert disabled["ok"] and enabled["ok"] and plugin(hidden, plugin_id)["worker_pid"] is None, hidden
        assert all(item["plugin"] != plugin_id for point in ("importers", "exporters") for item in hidden[point]), hidden
        assert [item["metadata"]["extensions"] for item in visible["importers"] if item["plugin"] == plugin_id] == [
            [".csv"], [".ssv"],
        ]
        assert [item["metadata"]["extensions"] for item in visible["exporters"] if item["plugin"] == plugin_id] == [[".csv"]]
        assert log.read_text().splitlines() == previous + ["activate", "deactivate"]

    assert run(host, profile, "import", sample, local_id)["table"] == local["table"]
    assert run(host, profile, "import", sample, worker_id)["table"] == worker["table"]
    check_error(run(host, profile, "install", package, local_digest, "worker_process", "status"), "already_installed")
    denied_profile = scratch / "denied profile"
    assert run(host, denied_profile, "install", package, local_digest, "in_process")["ok"]
    check_error(run(host, denied_profile, "import", sample, local_id), "service_denied")
    assert run(host, denied_profile, "install", worker_csv, worker_digest, "worker_process")["ok"]
    check_error(run(host, denied_profile, "import", sample, worker_id), "service_denied")

    wrong_abi = scratch / "wrong ABI"
    shutil.copytree(package, wrong_abi)
    manifest_path = wrong_abi / "plugin.json"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    manifest["cpp_abi"] = "incompatible-build"
    manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
    wrong_digest = run(host, denied_profile, "inspect", wrong_abi)["digest"]
    check_error(run(host, denied_profile, "install", wrong_abi, wrong_digest, "in_process"), "contract_mismatch")


def verify_streams(host, package, worker_csv, faults, scratch):
    source = scratch / "large streamed file.ssv"
    limit = 512 * 1024
    # Split CRLF, a doubled quote, and a UTF-8 character across distinct read boundaries.
    first = b"x" * (3 * limit - len(b"a;b\r\n") - len(b";1\r")) + b";1\r\n"
    second = b'"' + b"y" * (limit - len(b"\n") - len(b'""')) + b'""z";2\r\n'
    offset = (len(b"a;b\r\n") + len(first) + len(second)) % limit
    third = b"z" * (limit - offset - 1) + '上海;3\r\n'.encode("utf-8")
    row = b'"a ""quoted""; value";' + b"k" * 2048 + b"\r\n"
    count = 6000
    source.write_bytes(b"a;b\r\n" + first + second + third + row * count)
    reference = scratch / "reference output.csv"
    previous_limit = csv.field_size_limit(4 * limit)
    try:
        with source.open(encoding="utf-8", newline="") as text, reference.open("w", encoding="utf-8", newline="") as output:
            csv.writer(output, quoting=csv.QUOTE_ALL).writerows(csv.reader(text, delimiter=";"))
    finally:
        csv.field_size_limit(previous_limit)
    expected = reference.read_bytes()
    assert min(source.stat().st_size, len(expected)) > 8 * 1024 * 1024

    second_source = scratch / "second streamed file.ssv"
    second_cell = b"z" * 2048
    second_count = 4600
    second_source.write_bytes(b"c;d\n" + (b"other;" + second_cell + b"\n") * second_count)
    second_expected = b'"c","d"\r\n' + (b'"other","' + second_cell + b'"\r\n') * second_count
    assert min(second_source.stat().st_size, len(second_expected)) > 8 * 1024 * 1024
    small_first = scratch / "small first input.ssv"
    small_second = scratch / "small second input.ssv"
    small_first.write_bytes(b"first;value\n1;a\n")
    small_second.write_bytes(b"second;value\n2;b\n3;c\n")

    import_limit = 32 * 1024 * 1024
    at_limit = scratch / "at the import limit.csv"
    with at_limit.open("wb") as bounded:
        bounded.write(b"column\n")
        bounded.seek(import_limit - 1)
        bounded.write(b",")
    over_limit = scratch / "over the import limit.csv"
    with over_limit.open("wb") as oversized:
        oversized.truncate(import_limit + 1)

    for csv_package, mode in ((package, "in_process"), (worker_csv, "worker_process")):
        profile = scratch / f"streams {mode}"
        info = run(host, profile, "inspect", csv_package)
        fault = run(host, profile, "inspect", faults)
        plugin_id = info["id"]
        assert run(host, profile, "install", csv_package, info["digest"], mode, "status")["ok"]
        assert run(host, profile, "install", faults, fault["digest"], mode)["ok"]
        assert run(host, profile, "check-catalog-events", plugin_id) == {"ok": True, "catalog_events": True}
        check_error(run(host, profile, "import", over_limit, plugin_id), "payload_too_large")
        imported = run(host, profile, "import", at_limit, plugin_id, "15000")
        assert imported["ok"] and imported["issues"][0]["code"] == "csv.column_count", imported
        for kind in ("sync", "async"):
            destination = scratch / f"{mode} {kind}.csv"
            destination.write_bytes(b"previous contents")
            pid, result = run(host, profile, commands=[
                ("pid",), ("convert", kind, source, destination, plugin_id),
            ])
            assert result["ok"] and result["rows"] == count + 3, result
            assert result["input_bytes"] == source.stat().st_size and result["output_bytes"] == len(expected), result
            assert (result["plugin_pid"] == pid["pid"]) == (mode == "in_process"), result
            assert destination.read_bytes() == expected, (mode, kind)
            if kind == "async":
                assert result["progress_before_eof"] and result["progress_rows"] == count + 3, result
            if mode == "in_process":
                result = run(host, profile, "check-local-stream-reentry", plugin_id, kind)
                assert result == {"ok": True, "read": True, "write": True, "close": True}, result

        first_output = scratch / f"{mode} first pair.csv"
        second_output = scratch / f"{mode} second pair.csv"
        for kind in ("sync", "async"):
            pid, result = run(host, profile, commands=[
                ("pid",), ("convert-pair", kind, source, first_output, second_source, second_output, plugin_id),
            ])
            assert result["ok"] and result["separator"] == ";", result
            assert (result["plugin_pid"] == pid["pid"]) == (mode == "in_process"), result
            for slot, original, encoded, rows in (
                ("first", source, expected, count + 3), ("second", second_source, second_expected, second_count),
            ):
                assert result[slot] == {
                    "rows": rows, "input_bytes": original.stat().st_size, "output_bytes": len(encoded),
                    "progress_rows": rows if kind == "async" else None,
                    "progress_bytes": original.stat().st_size if kind == "async" else None,
                }, result
            assert first_output.read_bytes() == expected and second_output.read_bytes() == second_expected

        pair_args = ("convert-pair", "async", small_first, first_output, small_second, second_output)
        for omitted in ("first", "second"):
            result = run(host, profile, *pair_args, plugin_id, f"omit-{omitted}")
            assert result["ok"], result
            assert result["first"]["progress_rows"] == (None if omitted == "first" else 1), result
            assert result["second"]["progress_rows"] == (None if omitted == "second" else 2), result
        first_output.write_bytes(b"first retained")
        second_output.write_bytes(b"second retained")
        check_error(run(host, profile, *pair_args, plugin_id, "write-second"), "io")
        assert first_output.read_bytes() == b"first retained" and second_output.read_bytes() == b"second retained"

        if mode == "worker_process":
            for operation in ("read", "receiver", "idle"):
                result = run(host, profile, "check-worker-events", plugin_id, operation, fault["id"])
                assert result == {"ok": True, "recovered": True}, result
            assert run(host, profile, "check-clean-diagnostics", plugin_id) == {
                "ok": True, "installed": True, "clean": True,
            }

        destination = scratch / f"retained {mode}.csv"
        destination.write_bytes(b"retained")
        for operation in ("write", "close"):
            result = run(host, profile, "check-output-failure", "async", source, destination, plugin_id, operation)
            check_error(result, "io")
            assert destination.read_bytes() == b"retained"
        data = installed_data(profile, fault["id"])
        data.mkdir(parents=True, exist_ok=True)
        for flag, error in (("omit-output-close", "io"), ("fail-after-output-close", "plugin_failed")):
            (data / flag).touch()
            check_error(run(host, profile, "convert", "async", source, destination, fault["id"]), error)
            (data / flag).unlink()
            assert destination.read_bytes() == b"retained"
        (data / "omit-second-output-close").touch()
        check_error(run(host, profile, *pair_args, fault["id"]), "io")
        (data / "omit-second-output-close").unlink()
        assert first_output.read_bytes() == b"first retained" and second_output.read_bytes() == b"second retained"
        (data / "check-stale-stream").touch()
        assert run(host, profile, "check-stale-stream", fault["id"]) == {"ok": True, "old_source_reads": 0}
        for operation in ("read", "write"):
            for interruption in ("cancel", "deadline"):
                result = run(host, profile, "check-stream-interruption", plugin_id, operation, interruption)
                assert result == {"ok": True, "cancelled": True, "stop_requested": True,
                                  "deadline": interruption == "deadline"}, result
            if mode == "worker_process":
                result = run(host, profile, "check-sync-stream-deadline", plugin_id, operation)
                assert result == {"ok": True, "deadline_while_io_pending": True}, result
        assert not list(scratch.glob(".csv-export-*"))


def verify_factory_table(host, package, scratch, valid):
    source = scratch / "mismatched factory table"
    shutil.copytree(package, source)
    path = source / "plugin.json"
    manifest = json.loads(path.read_text(encoding="utf-8"))
    assert manifest["schema_version"] == 1 and "provides" not in manifest, manifest
    assert len(manifest["extensions"]) == 3, manifest
    assert all(set(extension) <= {"point", "metadata"} for extension in manifest["extensions"]), manifest
    exporter = manifest["extensions"][-1]
    profile = scratch / "manifest checks"
    target = manifest["target"]
    targets = ("windows-x86_64-msvc", "linux-x86_64-gnu", "linux-aarch64-gnu", "macos-arm64", "macos-x86_64")
    assert target in targets, target
    for other_target in targets:
        if other_target == target:
            continue
        manifest["target"] = other_target
        path.write_text(json.dumps(manifest), encoding="utf-8")
        check_error(run(host, profile, "inspect", source), "invalid_package")
    manifest["target"] = target
    for record, field, value in ((manifest, "provides", []), (exporter, "contract", "example.table-exporter"),
                                 (exporter, "id", "export")):
        record[field] = value
        path.write_text(json.dumps(manifest), encoding="utf-8")
        check_error(run(host, profile, "inspect", source), "invalid_package")
        del record[field]

    valid_metadata = exporter["metadata"]
    for invalid_metadata in (None, [], "csv", 5):
        exporter["metadata"] = invalid_metadata
        path.write_text(json.dumps(manifest), encoding="utf-8")
        check_error(run(host, profile, "inspect", source), "invalid_package")
    exporter["metadata"] = valid_metadata

    point = exporter["point"]
    exporter["point"] = "example.unregistered"
    path.write_text(json.dumps(manifest), encoding="utf-8")
    digest = run(host, profile, "inspect", source)["digest"]
    check_error(run(host, profile, "install", source, digest, "worker_process"), "contract_mismatch")

    exporter["point"] = point
    manifest["extensions"].append(exporter)
    path.write_text(json.dumps(manifest), encoding="utf-8")
    for mode in ("in_process", "worker_process"):
        profile = scratch / f"mismatched table {mode}"
        package_info = run(host, profile, "inspect", source)
        assert run(host, profile, "install", source, package_info["digest"], mode, "status")["ok"]
        check_error(run(host, profile, "import", valid, package_info["id"]), "contract_mismatch")
        assert not (installed_data(profile, package_info["id"]) / "lifecycle.log").exists()


def verify_reopened_profile(host, package, worker_csv, scratch, sample):
    sample_ssv = sample.with_suffix(".ssv")
    empty_exporter_package = scratch / "empty exporter metadata"
    shutil.copytree(worker_csv, empty_exporter_package)
    empty_manifest_path = empty_exporter_package / "plugin.json"
    empty_manifest = json.loads(empty_manifest_path.read_text(encoding="utf-8"))
    empty_manifest["extensions"][-1]["metadata"] = {}
    empty_manifest_path.write_text(json.dumps(empty_manifest), encoding="utf-8")
    csv_metadata = {
        "extensions": [".CSV"],
        "priority": 2**64 - 1,
        "options": {"dialect": {"delimiter": ",", "strict": True}, "weights": [-2, 0.25, None], "locale": "中文"},
    }
    for mode in ("in_process", "worker_process"):
        source = scratch / f"original package {mode}"
        shutil.copytree(package, source)
        path = source / "plugin.json"
        manifest = json.loads(path.read_text(encoding="utf-8"))
        manifest["extensions"][0]["metadata"] = csv_metadata
        del manifest["extensions"][-1]["metadata"]
        # Interleave the points while keeping the two importers in their exported order.
        manifest["extensions"][1], manifest["extensions"][2] = manifest["extensions"][2], manifest["extensions"][1]
        path.write_text(json.dumps(manifest), encoding="utf-8")
        profile = scratch / f"reopened {mode}"
        info = run(host, profile, "inspect", source)
        assert info["extensions"] == manifest["extensions"], info
        plugin_id = info["id"]
        empty = run(host, profile, "inspect", empty_exporter_package)
        assert empty["extensions"] == empty_manifest["extensions"], empty
        empty_id = empty["id"]
        expected_importers = [
            {"plugin": plugin_id, "metadata": entry["metadata"]} for entry in manifest["extensions"][::2]
        ] + [
            {"plugin": empty_id, "metadata": entry["metadata"]} for entry in empty_manifest["extensions"][:2]
        ]
        expected_exporters = [{"plugin": plugin_id}, {"plugin": empty_id, "metadata": {}}]
        installed, installed_empty, listed, imported, status = run(host, profile, commands=[
            ("install", source, info["digest"], mode, "status"),
            ("install", empty_exporter_package, empty["digest"], mode, "status"), ("list",),
            ("import", sample, plugin_id), ("status",),
        ])
        assert installed["ok"] and installed_empty["ok"], (installed, installed_empty)
        assert imported["ok"] and status["last_imported_rows"] == 2, (imported, status)
        assert listed["importers"] == expected_importers and listed["exporters"] == expected_exporters, listed
        log = installed_data(profile, plugin_id) / "lifecycle.log"
        lifecycle = ["activate", "deactivate"]
        assert log.read_text().splitlines() == lifecycle
        source.rename(scratch / f"moved package {mode}")

        mismatch = run(host, profile, "check-factory-contract", plugin_id)
        check_error(mismatch, "contract_mismatch")
        assert "example.table-exporters" in mismatch["message"], mismatch
        assert log.read_text().splitlines() == lifecycle

        listed, round_trip, status = run(host, profile, commands=[
            ("list",), ("round-trip", sample, sample_ssv, plugin_id), ("status",),
        ])
        entry = plugin(listed, plugin_id)
        assert entry["enabled"] and entry["digest"] == info["digest"] and entry["mode"] == mode, entry
        assert entry["extensions"] == manifest["extensions"], entry
        assert plugin(listed, empty_id)["extensions"] == empty_manifest["extensions"], listed
        assert listed["importers"] == expected_importers and listed["exporters"] == expected_exporters, listed
        assert round_trip["ok"] and round_trip["package_importers"] == 2 and round_trip["format"] == "csv", round_trip
        assert status["last_imported_rows"] == 2 and log.read_text().splitlines() == lifecycle * 2, status


def verify_package_resources(host, package, scratch, sample, module_name):
    source = scratch / "resource package"
    shutil.copytree(package, source)
    profile = scratch / "resource profile"
    payload = next((source / "resources").rglob("cell-limit.txt"))
    relative = payload.relative_to(source)
    info = run(host, profile, "inspect", source)
    files = [source / "plugin.json", source / module_name]
    files.extend(path for path in (source / "resources").rglob("*") if path.is_file())
    expected = hashlib.sha256()
    for path in sorted(files, key=lambda path: path.relative_to(source).as_posix().encode("utf-8")):
        expected.update(path.relative_to(source).as_posix().encode("utf-8") + b"\0")
        expected.update(hashlib.sha256(path.read_bytes()).hexdigest().encode("ascii"))
    assert info["digest"] == expected.hexdigest(), info

    original = payload.read_bytes()
    payload.write_bytes(b"40\n")
    check_error(run(host, profile, "install", source, info["digest"], "worker_process", "status"), "approval_mismatch")
    changed = run(host, profile, "inspect", source)
    assert changed["digest"] != info["digest"], changed
    payload.write_bytes(original)
    payload.rename(payload.with_suffix(".renamed"))
    renamed = run(host, profile, "inspect", source)
    assert renamed["digest"] not in (changed["digest"], info["digest"]), renamed
    payload.with_suffix(".renamed").rename(payload)

    for mode in ("in_process", "worker_process"):
        profile = scratch / f"modified resources {mode}"
        assert run(host, profile, "install", source, info["digest"], mode, "status")["ok"]
        installed = profile / "packages" / info["digest"] / relative
        installed.write_bytes(b"40\n")
        check_error(run(host, profile, "import", sample, info["id"]), "package_changed")
        installed.unlink()
        check_error(run(host, profile, "import", sample, info["id"]), "package_changed")
        abandoned = profile / "packages" / ("staging-" + "0" * 64)
        abandoned.mkdir()
        assert run(host, profile, "install", source, info["digest"], mode, "status")["ok"]
        assert installed.read_bytes() == original and not abandoned.exists()
        assert run(host, profile, "import", sample, info["id"])["table"] is not None


def verify_settings(host, package, scratch, sample):
    info = run(host, scratch / "settings inspection", "inspect", package)
    plugin_id = info["id"]
    assert info["settings_schema"][0]["title"] == {"resource": "csv_resources:strings/settings_max_rows"}, info
    reset = scratch / "reset settings.json"
    reset.write_text("{}", encoding="utf-8")
    invalid = scratch / "invalid settings.json"
    invalid.write_text('{"max_rows": 0, "unknown": true}', encoding="utf-8")
    for mode in ("in_process", "worker_process"):
        profile = scratch / f"settings {mode}"
        assert run(host, profile, "install", package, info["digest"], mode, "status")["ok"]
        assert run(host, profile, "disable", plugin_id)["ok"]
        english, chinese, fallback, listed = run(host, profile, commands=[
            ("settings", plugin_id, "en-US"), ("settings", plugin_id, "zh-Hans-CN"),
            ("settings", plugin_id, "fr-CA"), ("list",),
        ])
        assert not plugin(listed, plugin_id)["enabled"] and plugin(listed, plugin_id)["worker_pid"] is None, listed
        assert not installed_data(profile, plugin_id).exists(), listed
        assert [view["schema"][0]["title"] for view in (english, chinese, fallback)] == [
            "Maximum rows", "最大行数", "Maximum rows",
        ], (english, chinese, fallback)
        for view in (english, chinese, fallback):
            assert view["schema"][0]["description"] == "Count data rows after the header.", view
            assert view["effective"] == {"max_rows": 10000} and view["overrides"] == {}, view
            assert view["revision"] == english["revision"] and view["active_revision"] is None, view

        rejected = run(host, profile, "set-settings", plugin_id, english["revision"], invalid)
        check_error(rejected, "invalid_argument")
        assert rejected["fields"] == [{"key": "max_rows", "reason": "out_of_range"},
                                      {"key": "unknown", "reason": "unknown_key"}], rejected
        assert run(host, profile, "settings", plugin_id, "en-US") == english
        assert run(host, profile, "enable", plugin_id)["ok"]
        activated = run(host, profile, "check-settings-activation", plugin_id)
        assert activated["effective"] == activated["overrides"] == {"max_rows": 1}, activated
        first_revision = activated["revision"]
        before, saved, still_old, stale, _, disabled, _, after, current = run(host, profile, commands=[
            ("import", sample, plugin_id), ("set-settings", plugin_id, first_revision, reset),
            ("import", sample, plugin_id), ("set-settings", plugin_id, first_revision, reset),
            ("disable", plugin_id), ("settings", plugin_id, "zh-Hans-CN"),
            ("enable", plugin_id), ("import", sample, plugin_id), ("settings", plugin_id),
        ])
        for limited in (before, still_old):
            assert limited["table"] is None and limited["issues"][0]["code"] == "csv.row_limit", limited
        assert saved["active_revision"] == first_revision and saved["revision"] != first_revision, saved
        assert saved["effective"] == {"max_rows": 10000} and saved["overrides"] == {}, saved
        check_error(stale, "settings_changed")
        assert disabled["active_revision"] is None and disabled["schema"][0]["title"] == "最大行数", disabled
        assert after["table"] and after["progress_rows"] == 2, after
        assert current["active_revision"] == current["revision"] == saved["revision"], current

    missing = scratch / "missing setting resource"
    shutil.copytree(package, missing)
    manifest_path = missing / "plugin.json"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    manifest["settings"][0]["title"]["resource"] = "csv_resources:strings/missing"
    manifest["version"] = "0.2.0"
    manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
    for command in (("inspect", missing), ("install", missing, info["digest"], "worker_process"),
                    ("upgrade", missing, info["digest"], info["digest"])):
        bad = run(host, profile, *command)
        check_error(bad, "invalid_package")
        assert "settings.max_rows.title" in bad["message"] and "csv_resources:strings/missing" in bad["message"], bad


def verify_settings_upgrade(host, faults, replacement, scratch):
    def customize(source, name, upgrading):
        package = scratch / name
        shutil.copytree(source, package)
        path = package / "plugin.json"
        manifest = json.loads(path.read_text(encoding="utf-8"))
        settings = manifest["settings"]
        settings.extend([
            {"key": "flag", "type": "boolean", "title": "$5", "default": False},
            {"key": "label", "type": "string", "title": "Label", "default": ""},
        ])
        if upgrading:
            settings[0].update(maximum=50, default=20)
            settings.append({"key": "style", "type": "string", "title": "Style", "default": "new"})
        else:
            settings.append({"key": "style", "type": "enum", "title": "Style", "default": "original", "options": [
                {"value": "original", "title": "Original"},
                {"value": "alternate", "title": {"resource": "csv_resources:strings/settings_max_rows"}},
            ]})
        path.write_text(json.dumps(manifest), encoding="utf-8")
        return package

    old_package = customize(faults, "settings upgrade source", False)
    new_package = customize(replacement, "settings upgrade candidate", True)
    inspect = scratch / "settings upgrade inspection"
    old_info = run(host, inspect, "inspect", old_package)
    new_info = run(host, inspect, "inspect", new_package)
    plugin_id = old_info["id"]
    old = old_info["digest"]
    new = new_info["digest"]
    edits = scratch / "settings upgrade edits.json"
    values = {"max_rows": 75, "flag": True, "label": "自定义文案", "style": "alternate"}
    edits.write_text(json.dumps(values, ensure_ascii=False), encoding="utf-8")
    updated = scratch / "settings after staging.json"
    updated.write_text(json.dumps({**values, "label": "重启前修改"}, ensure_ascii=False), encoding="utf-8")
    request = scratch / "settings values.csv"
    request.write_bytes(b"settings")
    for mode in ("worker_process", "in_process"):
        profile = scratch / f"settings upgrade {mode}"
        assert run(host, profile, "install", old_package, old, mode)["ok"]
        initial = run(host, profile, "settings", plugin_id, "zh-Hans-CN")
        assert initial["schema"][1]["title"] == "$5", initial
        assert initial["schema"][-1]["options"][-1] == {"value": "alternate", "title": "最大行数"}, initial
        saved = run(host, profile, "set-settings", plugin_id, initial["revision"], edits)
        declared = run(host, profile, "import", request, plugin_id)
        assert dict(declared["table"]["rows"]) == {"max_rows": "75", "flag": "true", "label": "自定义文案",
                                                  "style": "alternate"}, declared
        if mode == "in_process":
            staged, saved = run(host, profile, commands=[
                ("upgrade", new_package, old, new), ("set-settings", plugin_id, saved["revision"], updated),
            ])
            assert staged["result"] == "restart_required" and saved["overrides"]["label"] == "重启前修改", staged
        else:
            failed = installed_data(profile, plugin_id) / "fail-upgrade"
            failed.touch()
            check_error(run(host, profile, "upgrade", new_package, old, new), "plugin_failed")
            persisted = run(host, profile, "settings", plugin_id)
            assert persisted["overrides"] == values and persisted["revision"] == saved["revision"], persisted
            failed.unlink()
            assert run(host, profile, "upgrade", new_package, old, new)["result"] == "applied"
        imported, migrated = run(host, profile, commands=[("import", request, plugin_id), ("settings", plugin_id)])
        label = saved["overrides"]["label"]
        assert migrated["overrides"] == {"flag": True, "label": label}, migrated
        assert migrated["effective"] == {"max_rows": 20, "flag": True, "label": label, "style": "new"}, migrated
        assert migrated["schema"][0]["title"] == "Maximum rows per import", migrated
        assert migrated["active_revision"] == migrated["revision"] != saved["revision"], migrated
        assert dict(imported["table"]["rows"]) == {"max_rows": "20", "flag": "true", "label": label,
                                                  "style": "new"}, imported


def verify_suffix_metadata(host, package, scratch, sample):
    source = scratch / "CSV suffix metadata"
    shutil.copytree(package, source)
    path = source / "plugin.json"
    manifest = json.loads(path.read_text(encoding="utf-8"))
    manifest["extensions"][0]["metadata"] = {"extensions": [".csv", 42]}
    manifest["extensions"][1]["metadata"] = {"extensions": {"label": "semicolon"}, "priority": 0}
    manifest["extensions"][-1]["metadata"] = {}
    path.write_text(json.dumps(manifest), encoding="utf-8")
    profile = scratch / "CSV suffix policy"
    info = run(host, profile, "inspect", source)
    assert info["ok"] and info["extensions"] == manifest["extensions"], info
    plugin_id = info["id"]
    installed, listed, csv, ssv = run(host, profile, commands=[
        ("install", source, info["digest"], "worker_process", "status"), ("list",),
        ("import", sample, plugin_id), ("import", sample.with_suffix(".ssv"), plugin_id),
    ])
    assert installed["ok"], installed
    assert [entry["metadata"] for entry in listed["importers"]] == [
        entry["metadata"] for entry in manifest["extensions"][:2]
    ], listed
    assert listed["exporters"] == [{"plugin": plugin_id, "metadata": {}}], listed
    assert plugin(listed, plugin_id)["worker_pid"] is None, listed
    check_error(csv, "not_found")
    check_error(ssv, "not_found")
    assert not (installed_data(profile, plugin_id) / "lifecycle.log").exists()


def verify_local_lifetime(host, faults, digest, fault_id, scratch, valid):
    profile = scratch / "local lifetime"
    assert run(host, profile, "install", faults, digest, "in_process", "status")["ok"]
    lifetime, recovered = run(host, profile, commands=[
        ("check-receiver-lifetime", fault_id),
        ("import", valid, fault_id),
    ])
    assert lifetime["ok"] and recovered["ok"], (lifetime, recovered)
    assert lifetime["cancelled_released"] and lifetime["late_rows"] == 0, lifetime
    assert any(item["plugin"] == fault_id for item in lifetime["catalog_during_callback"]["importers"]), lifetime
    waiting = scratch / "background stop.csv"
    waiting.write_bytes(b"background-stop")
    log = profile / "data" / f"plugin-{fault_id}.data" / "lifecycle.log"
    replies = run(host, profile, commands=[
        ("import", waiting, fault_id, 200),
        ("disable", fault_id),
        ("list",),
    ])
    check_error(replies[0], "deadline_exceeded")
    assert replies[1]["ok"] and not replies[2]["importers"], replies[1:]
    assert log.read_text().splitlines()[-4:] == [
        "activate", "background-start", "background-stopped", "deactivate",
    ], log.read_text()

    enabled, recovered = run(host, profile, commands=[("enable", fault_id), ("import", valid, fault_id)])
    assert enabled["ok"] and recovered["ok"], (enabled, recovered)


@contextmanager
def defer_data_cleanup(directory):
    if sys.platform == "win32":
        with (directory / "data-format").open("rb"):
            yield
    else:
        directory.chmod(0o500)
        try:
            yield
        finally:
            directory.chmod(0o700)


def verify_upgrade(host, package, faults, replacement, scratch, valid):
    plugin_id = "example.csv-faults"
    profile = scratch / "worker upgrade"
    old = run(host, profile, "inspect", faults)["digest"]
    new = run(host, profile, "inspect", replacement)["digest"]
    other = run(host, profile, "inspect", package)["digest"]
    check_error(run(host, profile, "upgrade", replacement, old, new), "not_found")
    assert run(host, profile, "install", faults, old, "worker_process", "status")["ok"]
    assert run(host, profile, "install", package, other, "worker_process", "status")["ok"]
    data = installed_data(profile, plugin_id)
    data.mkdir(parents=True)
    (data / "data-format").write_text("1", encoding="utf-8")
    (data / "retained.txt").write_text("user data", encoding="utf-8")
    check_error(run(host, profile, "upgrade", replacement, "0" * 64, new), "approval_mismatch")
    check_error(run(host, profile, "upgrade", replacement, old, "0" * 64), "approval_mismatch")
    for failure in ("fail-upgrade", "fail-exporter"):
        failed = data / failure
        failed.touch()
        broken, unchanged, context = run(host, profile, commands=[
            ("check-upgrade-failure-events", replacement, old, new), ("list",), ("check-plugin-context", plugin_id),
        ])
        assert broken == {"ok": True, "installed": old, "candidate": new, "restart_required": False,
                          "failed_at_activation": failure == "fail-upgrade",
                          "failed_at_factory": failure == "fail-exporter"}, broken
        assert plugin(unchanged, plugin_id)["digest"] == old and context["first"][-1] == "200000", (unchanged, context)
        assert (data / "data-format").read_text() == "1" and set((profile / "data").iterdir()) == {data}
        assert not (profile / "packages" / new).exists()
        failed.unlink()

    status_request = scratch / "upgrade status.csv"
    status_request.write_bytes(b"status")
    before, changed, after, context, denied = run(host, profile, commands=[
        ("import", valid, "example.csv"), ("check-worker-upgrade", replacement, old, new),
        ("import", valid, "example.csv"), ("check-plugin-context", plugin_id), ("import", status_request, plugin_id),
    ])
    assert changed["ok"] and changed["old_pid"] != changed["new_pid"], changed
    assert before["ok"] and after["ok"] and before["plugin_pid"] == after["plugin_pid"], (before, after)
    assert context["first"][1:3] == ["0.2.0", new], context
    assert context["first"][-1] == "300000", context
    check_error(denied, "service_denied")
    migrated = installed_data(profile, plugin_id)
    assert migrated != data and not data.exists() and not (profile / "packages" / old).exists()
    assert (migrated / "data-format").read_text() == "2"
    assert (migrated / "retained.txt").read_text() == "user data"

    local = scratch / "in-process upgrade"
    assert run(host, local, "install", faults, old, "in_process")["ok"]
    data = installed_data(local, plugin_id)
    data.mkdir(parents=True)
    (data / "data-format").write_text("1", encoding="utf-8")
    assert run(host, local, "check-in-process-upgrade", replacement, old, new) == {
        "ok": True, "result": "restart_required", "current_digest": old, "pending_digest": new,
    }
    assert (data / "data-format").read_text() == "1"
    (data / "last-write.txt").write_text("before restart", encoding="utf-8")
    context = run(host, local, "check-plugin-context", plugin_id)
    assert context["first"][1:3] == ["0.2.0", new], context
    assert context["first"][-1] == "300000", context
    migrated = installed_data(local, plugin_id)
    assert (migrated / "last-write.txt").read_text() == "before restart"
    assert (migrated / "data-format").read_text() == "2" and not data.exists()

    rejected = scratch / "failed in-process upgrade"
    assert run(host, rejected, "install", faults, old, "in_process")["ok"]
    data = installed_data(rejected, plugin_id)
    data.mkdir(parents=True)
    (data / "data-format").write_text("1", encoding="utf-8")
    (data / "fail-exporter").touch()
    assert run(host, rejected, "upgrade", replacement, old, new)["result"] == "restart_required"
    assert run(host, rejected, "check-pending-upgrade-events", plugin_id) == {
        "ok": True, "installed": old, "candidate": new, "restart_required": True,
        "failed_at_activation": False, "failed_at_factory": True,
    }
    assert (data / "data-format").read_text() == "1"
    recovered = run(host, rejected, "check-plugin-context", plugin_id)
    assert recovered["first"][1:3] == ["0.1.0", old], recovered
    assert recovered["first"][-1] == "200000", recovered
    assert not (rejected / "packages" / new).exists()

    disabled = scratch / "disabled upgrade"
    assert run(host, disabled, "install", faults, old, "worker_process")["ok"]
    assert run(host, disabled, "disable", plugin_id)["ok"]
    data = installed_data(disabled, plugin_id)
    data.mkdir(parents=True)
    (data / "data-format").write_text("1", encoding="utf-8")
    assert run(host, disabled, "upgrade", replacement, old, new)["result"] == "applied"
    listed = run(host, disabled, "list")
    assert not listed["importers"] and not listed["exporters"] and not plugin(listed, plugin_id)["enabled"], listed
    assert (installed_data(disabled, plugin_id) / "data-format").read_text() == "1"
    assert run(host, disabled, "enable", plugin_id)["ok"]
    assert run(host, disabled, "import", valid, plugin_id)["ok"]
    assert (installed_data(disabled, plugin_id) / "data-format").read_text() == "2"


def verify_uninstall(host, faults, replacement, scratch, valid):
    plugin_id = "example.csv-faults"
    profile = scratch / "uninstall"
    old = run(host, profile, "inspect", faults)["digest"]
    new = run(host, profile, "inspect", replacement)["digest"]
    assert run(host, profile, "install", faults, old, "worker_process", "status")["ok"]
    settings = run(host, profile, "settings", plugin_id)
    overrides = scratch / "uninstall settings.json"
    overrides.write_text('{"max_rows": 2}', encoding="utf-8")
    assert run(host, profile, "set-settings", plugin_id, settings["revision"], overrides)["ok"]
    data = installed_data(profile, plugin_id)
    data.mkdir(parents=True)
    (data.parent / "unrelated.txt").write_text("outside plugin data", encoding="utf-8")
    (data / "retained.txt").write_text("user data", encoding="utf-8")
    check_error(run(host, profile, "uninstall", plugin_id, new), "approval_mismatch")
    imported, removed, listed = run(host, profile, commands=[
        ("import", valid, plugin_id), ("uninstall", plugin_id, old), ("list",),
    ])
    assert imported["ok"] and removed["result"] == "removed", (imported, removed)
    assert not listed["plugins"] and not listed["importers"] and not listed["exporters"], listed
    assert not (profile / "packages" / old).exists() and (data / "retained.txt").exists()
    assert run(host, profile, "install", faults, old, "worker_process")["ok"]
    assert installed_data(profile, plugin_id) == data
    assert run(host, profile, "settings", plugin_id)["overrides"] == {"max_rows": 2}
    status_request = scratch / "reinstall status.csv"
    status_request.write_bytes(b"status")
    check_error(run(host, profile, "import", status_request, plugin_id), "service_denied")
    assert run(host, profile, "uninstall", plugin_id, old, "delete-data")["result"] == "removed"
    assert not data.exists()
    assert (data.parent / "unrelated.txt").read_text() == "outside plugin data"
    assert run(host, profile, "install", faults, old, "worker_process")["ok"]
    assert run(host, profile, "settings", plugin_id)["overrides"] == {}
    assert run(host, profile, "import", valid, plugin_id)["ok"]
    assert not (installed_data(profile, plugin_id) / "retained.txt").exists()

    local = scratch / "uninstall pending upgrade"
    assert run(host, local, "install", faults, old, "in_process")["ok"]
    imported, staged, removed, listed = run(host, local, commands=[
        ("import", valid, plugin_id), ("upgrade", replacement, old, new),
        ("uninstall", plugin_id, old), ("list",),
    ])
    assert imported["ok"] and staged["result"] == "restart_required", (imported, staged)
    assert removed["result"] == "cleanup_pending" and not listed["plugins"], (removed, listed)
    assert not (local / "packages" / new).exists()
    assert not run(host, local, "list")["plugins"]
    assert not (local / "packages" / old).exists()
    assert run(host, local, "install", faults, old, "in_process")["ok"]
    assert (installed_data(local, plugin_id) / "lifecycle.log").read_text().splitlines() == ["activate", "deactivate"]


def verify_interrupted_upgrade(host, faults, replacement, scratch):
    plugin_id = "example.csv-faults"
    old = run(host, scratch / "upgrade digests", "inspect", faults)["digest"]
    new = run(host, scratch / "upgrade digests", "inspect", replacement)["digest"]
    status_request = scratch / "interrupted upgrade status.csv"
    status_request.write_bytes(b"status")
    for mode in ("worker_process", "in_process"):
        profile = scratch / f"interrupted {mode} upgrade"
        assert run(host, profile, "install", faults, old, mode, "status")["ok"]
        settings = run(host, profile, "settings", plugin_id)
        overrides = scratch / "interrupted upgrade settings.json"
        overrides.write_text('{"max_rows": 12}', encoding="utf-8")
        saved = run(host, profile, "set-settings", plugin_id, settings["revision"], overrides)
        assert saved["ok"], saved
        data = installed_data(profile, plugin_id)
        data.mkdir(parents=True)
        (data / "data-format").write_text("1", encoding="utf-8")
        flag = data / ("pause-upgrade" if mode == "worker_process" else "exit-upgrade")
        flag.touch()
        upgrade = [str(host), str(profile), "upgrade", str(replacement), old, new]
        if mode == "worker_process":
            with subprocess.Popen(upgrade, stdout=subprocess.PIPE, stderr=subprocess.PIPE) as process:
                try:
                    deadline = time.monotonic() + 15
                    while not list((profile / "data").glob("*/upgrade-ready")):
                        assert process.poll() is None and time.monotonic() < deadline, "Candidate never started"
                        time.sleep(0.01)
                    process.kill()
                finally:
                    if process.poll() is None:
                        process.kill()
                    process.communicate(timeout=15)
        else:
            assert run(host, profile, "upgrade", replacement, old, new)["result"] == "restart_required"
            interrupted = subprocess.run([str(host), str(profile), "list"], capture_output=True, timeout=30)
            assert interrupted.returncode == 7, interrupted

        recovered = run(host, profile, "list")
        entry = plugin(recovered, plugin_id)
        assert entry["digest"] == old and entry["pending_upgrade"] is None, recovered
        assert run(host, profile, "settings", plugin_id) == saved
        assert (data / "data-format").read_text() == "1" and set((profile / "data").iterdir()) == {data}
        assert not (profile / "packages" / new).exists()
        imported, status, context = run(host, profile, commands=[
            ("import", status_request, plugin_id), ("status",), ("check-plugin-context", plugin_id),
        ])
        assert imported["ok"] and status["last_imported_rows"] == 41, (imported, status)
        assert context["first"][-1] == "200000", context
        flag.unlink()

    profile = scratch / "committed upgrade"
    assert run(host, profile, "install", faults, old, "worker_process", "status")["ok"]
    data = installed_data(profile, plugin_id)
    data.mkdir(parents=True)
    (data / "data-format").write_text("1", encoding="utf-8")
    state_file = profile / "profile.json"
    if sys.platform == "win32" or os.geteuid() != 0:
        with defer_data_cleanup(data):
            with subprocess.Popen([str(host), str(profile)], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=subprocess.PIPE, text=True, encoding="utf-8") as process:
                try:
                    process.stdin.write(command_line(("upgrade", replacement, old, new)))
                    process.stdin.flush()
                    with ThreadPoolExecutor(max_workers=1) as reader:
                        reply = reader.submit(process.stdout.readline)
                        try:
                            assert json.loads(reply.result(timeout=15)) == {"ok": True, "result": "applied"}
                        except BaseException:
                            process.kill()
                            raise
                    state = json.loads(state_file.read_text(encoding="utf-8"))
                    assert state["plugins"][0]["digest"] == new and data.name in state["discarded_data"], state
                    process.kill()
                finally:
                    if process.poll() is None:
                        process.kill()
                    process.communicate(timeout=15)
    else:
        assert run(host, profile, "upgrade", replacement, old, new)["result"] == "applied"
        # Restore the files and cleanup entry that would remain before post-commit cleanup.
        shutil.copytree(faults, profile / "packages" / old)
        data.mkdir()
        (data / "data-format").write_text("1", encoding="utf-8")
        committed_state = json.loads(state_file.read_text(encoding="utf-8"))
        committed_state["discarded_data"].append(data.name)
        state_file.write_text(json.dumps(committed_state), encoding="utf-8")
    committed = run(host, profile, "list")
    assert plugin(committed, plugin_id)["digest"] == new and not data.exists(), committed
    assert not (profile / "packages" / old).exists()
    assert (installed_data(profile, plugin_id) / "data-format").read_text() == "2"
    check_error(run(host, profile, "import", status_request, plugin_id), "service_denied")


def verify(host, package, faults, worker_csv, replacement, scratch):
    assert run(host, scratch / "contract codecs", "check-contract-codecs") == {"ok": True}
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

    verify_settings(host, package, scratch, sample)
    verify_settings_upgrade(host, faults, replacement, scratch)

    csv_id = "example.csv"
    fault_id = "example.csv-faults"
    module_name = {"linux": "plugin.so", "darwin": "plugin.dylib", "win32": "plugin.dll"}[sys.platform]
    digest = run(host, profile, "inspect", package)["digest"]
    fault_digest = run(host, profile, "inspect", faults)["digest"]
    for mode in ("worker_process", "in_process"):
        verify_plugin_context(host, faults, fault_id, fault_digest, valid, scratch, mode)
    check_error(run(host, profile, "install", package, "0" * 64, "worker_process"), "approval_mismatch")

    replies = run(host, profile, commands=[
        ("install", package, digest, "worker_process", "status"),
        ("install", faults, fault_digest, "worker_process", "status"),
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
    assert all(reply["ok"] for reply in replies[:2]), replies[:2]
    (
        discovered, imported, active, invalid_result, exited, after_exit,
        restarted, expired, after_timeout, recovered, failure, disabled, after_disable,
    ) = replies[2:]
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
    assert run(host, profile, "import", valid, csv_id)["table"] == expected
    replies = run(host, profile, commands=[
        ("import", sample, csv_id),
        ("status",),
        ("list",),
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
        imported, status, active, reentered, unavailable, other_unavailable,
        before_timeout, expired, after_timeout, after_workers, restarted, after_restart,
    ) = replies
    assert imported["table"] == {
        "header": ["name", "city"], "rows": [["Chen, Lin", "Shanghai"], ["Noah", "New York"]],
    }
    assert status["last_imported_rows"] == 2, status
    assert imported["progress_rows"] == 2 and imported["delimiter"] == ",", imported
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
    assert run(host, modified_profile, "install", package, digest, "worker_process", "status")["ok"]
    installed = modified_profile / "packages" / digest / module_name
    with installed.open("ab") as module:
        module.write(b"changed")
    check_error(run(host, modified_profile, "import", valid, csv_id), "package_changed")
    assert run(host, modified_profile, "install", package, digest, "worker_process", "status")["ok"]
    repaired = run(host, modified_profile, "import", valid, csv_id)
    assert repaired["ok"] and repaired["table"] == expected

    verify_mixed(host, package, worker_csv, faults, scratch, sample, valid)
    verify_streams(host, package, worker_csv, faults, scratch)
    verify_factory_table(host, package, scratch, valid)
    verify_reopened_profile(host, package, worker_csv, scratch, sample)
    verify_package_resources(host, package, scratch, sample, module_name)
    verify_suffix_metadata(host, package, scratch, sample)
    verify_local_lifetime(host, faults, fault_digest, fault_id, scratch, valid)
    assert run(host, scratch / "in-process-only profile", "check-disable-during-create", package) == {
        "ok": True, "delimiter": ";",
    }
    verify_upgrade(host, package, faults, replacement, scratch, valid)
    verify_uninstall(host, faults, replacement, scratch, valid)
    verify_interrupted_upgrade(host, faults, replacement, scratch)

    if sys.platform in ("linux", "darwin"):
        verify_host_exit(host, profile, valid, hang_worker, fault_id, scratch)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Verify the standalone CSV host and separately built plugins.")
    parser.add_argument("host", type=Path)
    parser.add_argument("package", type=Path)
    parser.add_argument("faults", type=Path)
    parser.add_argument("worker_csv", type=Path)
    parser.add_argument("replacement", type=Path)
    parser.add_argument("--hpk", type=Path, help="Also verify hpk pack with the preview's published SDK")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="pluginkit csv ") as directory:
        host = args.host.resolve()
        scratch = Path(directory)
        if args.hpk:
            verify_pack(host, args.hpk.resolve(), scratch)
        verify(host, args.package.resolve(), args.faults.resolve(),
               args.worker_csv.resolve(), args.replacement.resolve(), scratch)
    print("CSV example passed")
