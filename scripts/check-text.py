"""Reject forbidden dash characters in tracked UTF-8 files."""

import pathlib
import subprocess
import sys


def main() -> int:
    root = pathlib.Path(__file__).resolve().parent.parent
    paths = subprocess.check_output(["git", "ls-files", "-z"], cwd=root).split(b"\0")
    failed = False
    for raw in filter(None, paths):
        path = root / raw.decode("utf-8")
        try:
            content = path.read_text(encoding="utf-8")
        except UnicodeDecodeError:
            continue
        for number, line in enumerate(content.splitlines(), 1):
            if any(char in line for char in (chr(0x2013), chr(0x2014))):
                print(f"{path.relative_to(root)}:{number}: forbidden dash character")
                failed = True
    return int(failed)


if __name__ == "__main__":
    sys.exit(main())
