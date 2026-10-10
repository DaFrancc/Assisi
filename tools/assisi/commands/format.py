"""format: run uncrustify over the tracked C++ sources."""

from __future__ import annotations

import os
import tempfile

from assisi.options import Kind, Option

NAME = "format"
HELP = "Format the C++ sources with uncrustify; --check only reports what would change"
OPTIONS = (Option("check", Kind.FLAG, "change nothing; fail if anything would be reformatted"),)
RECORDED = True

PATTERNS = ("*.cpp", "*.hpp", "*.h", "*.cc", "*.hxx")

# The reflectgen golden files are compared byte for byte against generator output,
# and its fixtures are that generator's input, so formatting either breaks its test.
EXCLUDED = ("tools/reflectgen/tests/golden/", "tools/reflectgen/tests/fixtures/")

CONFIG = ".uncrustify.cfg"

# A first pass over unformatted source can leave a few files one pass short of a
# fixed point, which a check then fails; a second pass reaches it.
FORMAT_PASSES = 2


def execute(request, context) -> int:
    listed = context.runner.capture(["git", "ls-files", *PATTERNS]).splitlines()
    files = [path for path in listed if path and not path.startswith(EXCLUDED)]
    # Through a list file rather than the command line, which on Windows is
    # limited to a few thousand characters and the source tree is not.
    handle, list_path = tempfile.mkstemp(prefix="assisi-format-", suffix=".txt")
    try:
        with os.fdopen(handle, "w") as listing:
            listing.write("\n".join(files) + "\n")
        base = ["uncrustify", "-c", CONFIG, "-l", "CPP"]
        if request.get("check"):
            context.runner.run(base + ["--check", "-F", list_path])
        else:
            for _ in range(FORMAT_PASSES):
                context.runner.run(base + ["--no-backup", "-F", list_path])
    finally:
        os.unlink(list_path)
    return 0
