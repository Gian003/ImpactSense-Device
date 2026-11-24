"""Stamps the running git revision into the firmware at build time.

firmware_version used to be typed by hand into the SET provisioning command,
which meant a unit could report "2.0.1" while running something else
entirely - nothing checked it, and nothing could. The pre-final defence panel
asked for device_code, model and firmware_version to be assigned
automatically; firmware_version is the one with an answer that needs no
server, no network and no operator.

Produces something like:

    6fe3c36            a clean build of that commit
    6fe3c36-dirty      built with uncommitted changes in the working tree
    v1.2.0-3-gabc123   three commits past tag v1.2.0

The "-dirty" suffix is the point. When a unit misbehaves in the field, it
tells you whether the binary on it corresponds to anything in the repository
at all - which is exactly the question you cannot answer today.

Wired up by `extra_scripts = pre:inject_version.py` in platformio.ini.
"""

import subprocess

Import("env")  # noqa: F821 - injected by SCons


def git_revision() -> str:
    """The current revision, or "unknown" if it cannot be determined.

    Never raises. A missing version string is not worth failing a build
    over - someone building from a source zip with no .git directory should
    still get a working binary, and device_identity.cpp carries its own
    "unknown" fallback for exactly that case.
    """
    try:
        out = subprocess.check_output(
            ["git", "describe", "--tags", "--always", "--dirty"],
            cwd=env["PROJECT_DIR"],  # noqa: F821
            stderr=subprocess.DEVNULL,
        )
    except Exception:
        # No git on PATH, not a repository, or a repository with no commits
        # yet. All three mean the same thing here.
        return "unknown"

    return out.strip().decode("utf-8", "replace") or "unknown"


revision = git_revision()

# StringifyMacro handles the quoting, which is worth not doing by hand: a
# bare -DFIRMWARE_VERSION=v1.2.0 is a syntax error in C, and escaping it
# correctly differs between shells.
env.Append(CPPDEFINES=[("FIRMWARE_VERSION", env.StringifyMacro(revision))])  # noqa: F821

print(f"firmware_version = {revision}")
