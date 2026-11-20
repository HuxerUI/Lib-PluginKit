"""Run the existing CSV command host after APK installation and external package import."""

import argparse
import hashlib
import json
import shlex
import shutil
import subprocess
import tempfile
import time
import uuid
from pathlib import Path

APP = "org.huxerui.lib.pluginkit.preview"
REMOTE = f"/data/user/0/{APP}/files/pluginkit-csv"


class Device:
    def __init__(self, serial):
        self.command = ["adb", "-s", serial]
        self.counter = 0
        token = uuid.uuid4().hex
        self.remote = REMOTE + "/" + token
        self.temporary = "/data/local/tmp/pluginkit-csv-" + token

    def adb(self, *args, check=True, timeout=30):
        return subprocess.run([*self.command, *map(str, args)], check=check, capture_output=True,
                              text=True, encoding="utf-8", errors="replace", timeout=timeout)

    def app(self, command, check=True):
        return self.adb("shell", "run-as", APP, "sh", "-c", shlex.quote(command), check=check)

    def upload(self, source, name):
        temporary = f"{self.temporary}/{name}"
        self.adb("shell", "mkdir", "-p", self.temporary)
        self.adb("push", source, temporary)
        target = f"{self.remote}/{name}"
        self.app(f"mkdir -p {shlex.quote(self.remote)}; cp -R {shlex.quote(temporary)} {shlex.quote(target)}")
        return target

    def run(self, *commands):
        self.counter += 1
        with tempfile.TemporaryDirectory(prefix="pluginkit android ") as temporary:
            script = Path(temporary) / "commands.txt"
            script.write_text("".join(" ".join(json.dumps(str(arg), ensure_ascii=False) for arg in command) + "\n"
                                      for command in commands), encoding="utf-8")
            file = self.upload(script, f"commands-{self.counter}")
        self.adb("shell", "am", "force-stop", APP)
        self.adb("shell", "am", "start", "-n", f"{APP}/.MainActivity", "--es", "commands", file)
        deadline = time.monotonic() + 90
        while time.monotonic() < deadline:
            if self.app(f"test -f {shlex.quote(file + '.done')}", check=False).returncode == 0:
                text = self.app(f"cat {shlex.quote(file + '.result')}").stdout
                replies = [json.loads(line) for line in text.splitlines() if line.strip()]
                assert len(replies) == len(commands), (commands, replies,
                    self.app(f"cat {shlex.quote(file + '.done')}").stdout,
                    self.app(f"cat {shlex.quote(file)}").stdout)
                return replies
            time.sleep(0.2)
        raise AssertionError(self.adb("logcat", "-d", "-t", "100").stdout)


def verify(device, packages, scratch, configuration):
    names = ("csv-importer", "csv-importer-worker", "csv-faults", "csv-faults-upgrade")
    paths = {name: device.upload(packages / name / configuration, name) for name in names}
    # Distinct CSV package IDs let the test fill the remaining slots before exercising sharing.
    pool_packages = {}
    for index in range(1, 4):
        name = f"csv-slot-{index}"
        package = scratch / name
        shutil.copytree(packages / "csv-importer-worker" / configuration, package)
        manifest = package / "plugin.json"
        values = json.loads(manifest.read_text(encoding="utf-8"))
        values["id"] = f"example.{name}"
        manifest.write_text(json.dumps(values), encoding="utf-8")
        pool_packages[name] = device.upload(package, name)
    paths.update(pool_packages)
    inspected = device.run(*[("inspect", path) for path in paths.values()])
    assert all(reply["ok"] for reply in inspected), inspected
    info = dict(zip(paths, inspected))
    replies = device.run(*[("install", paths[name], info[name]["digest"], "worker_process", "status")
                           for name in (*names[:3], *pool_packages)])
    assert all(reply["ok"] for reply in replies), replies

    sample = scratch / "sample.csv"
    sample.write_bytes(b"name,city\nAlice,Paris\n")
    remote_sample = device.upload(sample, sample.name)
    other_packages = [("import", remote_sample, info[name]["id"]) for name in pool_packages]

    def run_with_full_pool(leader, *commands):
        host, *replies = device.run(("pid",), ("import", remote_sample, leader), *other_packages, *commands)
        activated = replies[:4]
        assert all(reply["ok"] for reply in activated), activated
        pids = [reply["plugin_pid"] for reply in activated]
        assert len(set(pids)) == 4 and host["pid"] not in pids, (host, activated)
        return pids, replies[4:]

    _, (shared, reopened) = run_with_full_pool("example.csv",
        ("check-shared-worker", "example.csv", "example.csv-worker"), ("list",))
    assert shared["ok"] and shared["neighbor_survived"], shared
    assert reopened["ok"], reopened
    pids, (recovered, unaffected) = run_with_full_pool("example.csv-worker",
        ("check-shared-failure", "example.csv-worker", "example.csv-faults"), other_packages[0])
    assert recovered["ok"] and recovered["host_survived"], recovered
    assert recovered["old_pid"] == pids[0] and unaffected["ok"] and unaffected["plugin_pid"] == pids[1], unaffected

    # A process restart above reopens the saved profile without another installation.
    replies = device.run(("check-plugin-context", "example.csv-faults", "disable"),
                         ("settings", "example.csv-faults", "zh-Hans-CN"),
                         ("settings", "example.csv-faults", "en-US"),
                         ("enable", "example.csv-faults"))
    assert all(reply["ok"] for reply in replies), replies
    assert replies[1]["schema"][0]["title"] == "最大行数", replies
    assert replies[2]["schema"][0]["title"] == "Maximum rows", replies

    source = scratch / "large.ssv"
    row = b"a;" + b"x" * 1024 + b"\n"
    source.write_bytes(b"name;value\n" + row * 9000)
    assert source.stat().st_size > 8 * 1024 * 1024
    remote_source = device.upload(source, "large.ssv")
    expected = b'"name","value"\r\n' + (b'"a","' + b"x" * 1024 + b'"\r\n') * 9000
    expected_digest = hashlib.sha256(expected).hexdigest()
    for mode in ("worker_process", "in_process"):
        if mode == "in_process":
            removed = device.run(("uninstall", "example.csv", info["csv-importer"]["digest"]),
                                 ("install", paths["csv-importer"], info["csv-importer"]["digest"], mode, "status"))
            assert all(reply["ok"] for reply in removed), removed
        for kind in ("sync", "async"):
            destination = f"{device.remote}/{mode}-{kind}.csv"
            [result] = device.run(("convert", kind, remote_source, destination, "example.csv"))
            assert result["ok"] and result["rows"] == 9000 and result["output_bytes"] == len(expected), result
            if kind == "async":
                assert result["progress_rows"] == 9000 and result["progress_before_eof"], result
            actual = device.app(f"sha256sum {shlex.quote(destination)}").stdout.split()[0]
            assert actual == expected_digest, (mode, kind, actual)
            first = f"{device.remote}/{mode}-{kind}-first.csv"
            second = f"{device.remote}/{mode}-{kind}-second.csv"
            [pair] = device.run(("convert-pair", kind, remote_source, first, remote_source, second, "example.csv"))
            assert pair["ok"] and pair["first"]["rows"] == pair["second"]["rows"] == 9000, pair
            for output in (first, second):
                assert device.app(f"sha256sum {shlex.quote(output)}").stdout.split()[0] == expected_digest, pair

    state = json.loads(device.app(f"cat {device.remote}/profile/profile.json").stdout)
    installed = next(entry for entry in state["plugins"] if entry["manifest"]["id"] == "example.csv-faults")
    data = f"{device.remote}/profile/data/{installed['data']}"
    device.app(f"mkdir -p {data}; echo 1 > {data}/data-format; touch {data}/fail-upgrade")
    [failed] = device.run(("check-upgrade-failure-events", paths["csv-faults-upgrade"],
                          info["csv-faults"]["digest"], info["csv-faults-upgrade"]["digest"]))
    assert failed["ok"], failed
    assert device.app(f"cat {data}/data-format").stdout.strip() == "1"
    device.app(f"rm {data}/fail-upgrade")
    _, [upgraded] = run_with_full_pool("example.csv-worker",
        ("check-shared-upgrade", "example.csv-worker", paths["csv-faults-upgrade"],
         info["csv-faults"]["digest"], info["csv-faults-upgrade"]["digest"], "status"))
    assert upgraded["ok"] and upgraded["neighbor_survived"], upgraded
    state = json.loads(device.app(f"cat {device.remote}/profile/profile.json").stdout)
    installed = next(entry for entry in state["plugins"] if entry["manifest"]["id"] == "example.csv-faults")
    data = f"{device.remote}/profile/data/{installed['data']}"
    [removed] = device.run(("uninstall", "example.csv-faults", info["csv-faults-upgrade"]["digest"]))
    assert removed["ok"] and device.app(f"test -d {data}", check=False).returncode == 0, removed
    [restored] = device.run(("install", paths["csv-faults-upgrade"], info["csv-faults-upgrade"]["digest"],
                            "worker_process", "status"))
    assert restored["ok"], restored
    [deleted] = device.run(("uninstall", "example.csv-faults", info["csv-faults-upgrade"]["digest"], "delete-data"))
    assert deleted["ok"] and device.app(f"test -d {data}", check=False).returncode != 0, deleted
    print("Android CSV example passed")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("apk", type=Path)
    parser.add_argument("packages", type=Path, help="CMake packages directory")
    parser.add_argument("--serial", required=True)
    parser.add_argument("--config", choices=("debug", "release"), default="debug")
    args = parser.parse_args()
    device = Device(args.serial)
    device.adb("install", "-r", args.apk.resolve(), timeout=90)
    with tempfile.TemporaryDirectory(prefix="pluginkit android csv ") as temporary:
        try:
            verify(device, args.packages.resolve(), Path(temporary), args.config.capitalize())
        finally:
            try:
                device.adb("shell", "am", "force-stop", APP)
                device.app(f"rm -rf {shlex.quote(device.remote)}", check=False)
                device.adb("shell", "rm", "-rf", device.temporary, check=False)
            except subprocess.SubprocessError:
                pass
