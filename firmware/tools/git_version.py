"""platformio extra_scripts — inject the git build id as -DMWF_FW_BUILD.

`git describe --tags --dirty --always` gives "v0.1.0", "v0.1.0-3-g1a2b3c4" or
"v0.1.0-3-g1a2b3c4-dirty", so a running unit can report the exact commit it was
flashed from. Firmware falls back to a version constant when git is unavailable
(exported source, CI cache).
"""
import subprocess

Import("env")  # noqa: F821  (platformio construction environment)

try:
    desc = subprocess.check_output(
        ["git", "describe", "--tags", "--dirty", "--always"],
        cwd=env["PROJECT_DIR"], text=True, stderr=subprocess.DEVNULL).strip()
except Exception:  # noqa: BLE001 — any git failure -> header fallback
    desc = ""

if desc:
    env.Append(CPPDEFINES=[("MWF_FW_BUILD", env.StringifyMacro(desc))])
