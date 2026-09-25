"""Test-only helpers: where the emulator under test is and which volume it runs.

SIM36        the sim36 executable (default build/linux-make/sim36)
SIM36_VOLUME a System/36 volume image; the drivers skip without one

Nothing from the volume is stored in the repository: the default machine
definition is instantiated at run time from test/default-machine.sim.in.
"""
import os
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)


def exe():
    return os.environ.get("SIM36", os.path.join(ROOT, "build", "linux-make", "sim36"))


def monitor_path(path):
    """Return an absolute path safe to embed in a monitor command.

    The monitor lexer gives backslash its usual escape meaning.  Native
    Windows Python produces backslash-separated absolute paths, so normalize
    them to the forward slashes accepted by both the simulator and Windows.
    """
    return os.path.abspath(path).replace("\\", "/")


def volume():
    v = os.environ.get("SIM36_VOLUME", "")
    if not v or not os.path.exists(v):
        print("SKIP: no volume (set SIM36_VOLUME to a System/36 volume image)")
        sys.exit(77)
    return monitor_path(v)


def default_config(template="default-machine.sim.in", **subst):
    """Write the default machine definition for this volume; returns its path."""
    text = open(os.path.join(HERE, template)).read()
    values = {"@HERE@": ROOT, "@VOLUME@": volume()}
    values.update({"@%s@" % k.upper(): v for k, v in subst.items()})
    for key, value in values.items():
        text = text.replace(key, value)
    fd, path = tempfile.mkstemp(prefix="sim36-", suffix=".sim")
    with os.fdopen(fd, "w") as f:
        f.write(text)
    return path


def command(config=None, *extra):
    """The argument list that starts the emulator on the default machine."""
    return [exe(), "-c", config or default_config()] + list(extra)
