#!/usr/bin/env python3
"""Runs the native host with a framed message and checks the install reply."""
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile

if len(sys.argv) != 3:
    raise SystemExit("usage: test_native_host.py <host> <fake_steamcmd>")

HOST = os.path.abspath(sys.argv[1])
FAKE_STEAMCMD_SRC = os.path.abspath(sys.argv[2])


def frame(obj):
    data = json.dumps(obj).encode("utf-8")
    return struct.pack("<I", len(data)) + data


def run_host(message, env):
    proc = subprocess.run([HOST], input=frame(message), stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE, env=env, timeout=60)
    if len(proc.stdout) < 4:
        raise SystemExit("host returned no framed response; stderr=%r" % proc.stderr)
    length = struct.unpack("<I", proc.stdout[:4])[0]
    payload = proc.stdout[4:4 + length]
    if len(payload) != length:
        raise SystemExit("short native message")
    return json.loads(payload.decode("utf-8")), proc


with tempfile.TemporaryDirectory(prefix="pzmodmgr-native-") as tmp:
    home = os.path.join(tmp, "home")
    bin_dir = os.path.join(tmp, "bin")
    data_dir = os.path.join(home, "Zomboid", "pzmodmgr")
    os.makedirs(data_dir)
    os.makedirs(bin_dir)

    fake_steamcmd = os.path.join(tmp, "fake_steamcmd.sh")
    shutil.copyfile(FAKE_STEAMCMD_SRC, fake_steamcmd)
    os.chmod(fake_steamcmd, 0o755)

    fake_curl = os.path.join(bin_dir, "curl")
    with open(fake_curl, "w", encoding="utf-8") as out:
        out.write("""#!/bin/sh
case "$*" in
  *collectioncount*) printf '%s\\n' '{"response":{"result":1,"resultcount":0,"collectiondetails":[]}}' ;;
  *) printf '%s\\n' '{"response":{"result":1,"publishedfiledetails":[{"result":1,"time_updated":1700000000}]}}' ;;
esac
exit 0
""")
    os.chmod(fake_curl, 0o755)

    with open(os.path.join(data_dir, "config.ini"), "w", encoding="utf-8") as out:
        out.write("steamcmd_exe=%s\nsteam_user=test-user\n" % fake_steamcmd)

    env = dict(os.environ)
    env["HOME"] = home
    env["PATH"] = bin_dir + os.pathsep + env.get("PATH", "/usr/bin:/bin")
    env.pop("PZMODMGR_STEAMCMD", None)

    response, proc = run_host({"url": "123456789"}, env)
    if not response.get("ok"):
        raise SystemExit("add failed: %r stderr=%r" % (response, proc.stderr))
    if not os.path.exists(os.path.join(home, "Zomboid", "mods", "FixtureMod", "mod.info")):
        raise SystemExit("mod.info was not installed into the real mods layout")

    # The browser menu is a single add-or-update action: a second run on an
    # installed item must update/check it instead of failing as a duplicate.
    response, proc = run_host({"url": "123456789"}, env)
    if not response.get("ok"):
        raise SystemExit("installed item was not updated: %r stderr=%r" % (response, proc.stderr))
    if "Up to date" not in str(response.get("message", "")):
        raise SystemExit("installed item did not report up to date: %r" % (response,))

    response, proc = run_host({"url": "not-a-url"}, env)
    if response.get("ok"):
        raise SystemExit("invalid URL unexpectedly succeeded")

print("native host tests passed")
