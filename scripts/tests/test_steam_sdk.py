#!/usr/bin/env python3
"""Behavioural tests for steam_sdk.py.

What matters is what lands in the repository: the headers and the runtime
libraries the engine builds and ships with, and nothing else from the SDK — no
tools, no sample game, no API description — whichever form the download is in.
Then that a second run replaces the first, that a bad download leaves whatever
was there before untouched, and that an archive cannot write outside the folder.

The SDK here is a tree of placeholder files built by the test, laid out the way
Valve's zip is. No real SDK is needed or read.
"""

import subprocess
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path

SCRIPT = Path(__file__).resolve().parent.parent / "steam_sdk.py"
REPO = SCRIPT.parent.parent

# What a fake v1.65 SDK holds, relative to the folder the zip unpacks to. The
# first group is what the engine needs; the rest is what it must leave behind.
WANTED = [
    "sdk/public/steam/steam_api.h",
    "sdk/public/steam/isteamutils.h",
    "sdk/redistributable_bin/linux64/libsteam_api.so",
    "sdk/redistributable_bin/win64/steam_api64.dll",
    "sdk/redistributable_bin/win64/steam_api64.lib",
]
UNWANTED = [
    "sdk/Readme.txt",
    "sdk/public/steam/steam_api.json",
    "sdk/public/steam/lib/linux64/libsdkencryptedappticket.so",
    "sdk/redistributable_bin/steam_api.dll",
    "sdk/redistributable_bin/osx/libsteam_api.dylib",
    "sdk/tools/ContentBuilder/builder_linux/steamcmd.sh",
    "sdk/steamworksexample/Main.cpp",
    "sdk/glmgr/glmgr.h",
]

# What the destination holds after a good run, relative to it.
EXPECTED_TREE = sorted([
    "VERSION",
    "public/steam/isteamutils.h",
    "public/steam/steam_api.h",
    "redistributable_bin/linux64/libsteam_api.so",
    "redistributable_bin/win64/steam_api64.dll",
    "redistributable_bin/win64/steam_api64.lib",
])


def readme(version: str) -> str:
    """The head of the SDK's Readme.txt, which is where its version is written."""
    return ("================================================================\n\n"
            "Welcome to the Steamworks SDK.\n\n"
            "----------------------------------------------------------------\n"
            f"v{version} 23th July 2026\n"
            "----------------------------------------------------------------\n")


def make_sdk_folder(root: Path, version: str = "1.65", leave_out: str = "") -> Path:
    """Writes the fake SDK under root/steamworks_sdk and returns that folder."""
    top = root / "steamworks_sdk"
    for name in WANTED + UNWANTED:
        if name == leave_out:
            continue
        path = top / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(readme(version) if name.endswith("Readme.txt") else f"placeholder {name}\n")
    return top


def zip_folder(folder: Path, archive: Path, extra: dict[str, str] | None = None) -> Path:
    """Zips folder's contents with the paths Valve's zip uses (sdk/...)."""
    with zipfile.ZipFile(archive, "w") as out:
        for path in sorted(folder.rglob("*")):
            if path.is_file():
                out.write(path, path.relative_to(folder).as_posix())
        for name, text in (extra or {}).items():
            out.writestr(name, text)
    return archive


def tree(folder: Path) -> list[str]:
    return sorted(path.relative_to(folder).as_posix() for path in folder.rglob("*") if path.is_file())


class SteamSdkTest(unittest.TestCase):
    def setUp(self):
        self._directory = tempfile.TemporaryDirectory()
        self.directory = Path(self._directory.name)
        self.dest = self.directory / "repo" / "steamworks"

    def tearDown(self):
        self._directory.cleanup()

    def install(self, sdk: Path):
        return subprocess.run([sys.executable, "-I", str(SCRIPT), "--sdk", str(sdk), "--dest", str(self.dest)],
                              capture_output=True, text=True, check=False, stdin=subprocess.DEVNULL)

    def test_zip_yields_only_headers_and_libraries(self):
        archive = zip_folder(make_sdk_folder(self.directory), self.directory / "steamworks_sdk_165.zip")
        result = self.install(archive)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(tree(self.dest), EXPECTED_TREE)
        self.assertEqual((self.dest / "public/steam/steam_api.h").read_text(),
                         "placeholder sdk/public/steam/steam_api.h\n")

    def test_unpacked_folder_yields_the_same(self):
        result = self.install(make_sdk_folder(self.directory))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(tree(self.dest), EXPECTED_TREE)

    def test_sdk_folder_itself_yields_the_same(self):
        result = self.install(make_sdk_folder(self.directory) / "sdk")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(tree(self.dest), EXPECTED_TREE)

    def test_version_is_recorded(self):
        result = self.install(make_sdk_folder(self.directory))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual((self.dest / "VERSION").read_text().strip(), "1.65")

    def test_second_run_replaces_the_first(self):
        sdk = make_sdk_folder(self.directory)
        self.assertEqual(self.install(sdk).returncode, 0)
        stale = self.dest / "public/steam/isteamremovedinterface.h"
        stale.write_text("left over from an older SDK\n")
        result = self.install(sdk)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(tree(self.dest), EXPECTED_TREE)

    def test_download_without_the_api_header_is_refused_and_changes_nothing(self):
        self.assertEqual(self.install(make_sdk_folder(self.directory / "good")).returncode, 0)
        before = tree(self.dest)
        broken = make_sdk_folder(self.directory / "broken", leave_out="sdk/public/steam/steam_api.h")
        result = self.install(broken)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("steam_api.h", result.stderr)
        self.assertEqual(tree(self.dest), before)

    def test_download_without_the_api_header_leaves_no_folder(self):
        broken = make_sdk_folder(self.directory, leave_out="sdk/public/steam/steam_api.h")
        self.assertNotEqual(self.install(broken).returncode, 0)
        self.assertFalse(self.dest.exists())

    def test_archive_member_escaping_the_folder_is_refused(self):
        archive = zip_folder(make_sdk_folder(self.directory), self.directory / "steamworks_sdk_165.zip",
                             {"sdk/public/steam/../../../../escaped.h": "outside\n"})
        result = self.install(archive)
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(self.dest.exists())
        self.assertEqual([path for path in self.directory.rglob("escaped.h")], [])

    def test_older_sdk_is_refused_and_names_the_version_needed(self):
        result = self.install(make_sdk_folder(self.directory, version="1.60"))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("1.60", result.stderr)
        self.assertIn("1.65", result.stderr)
        self.assertFalse(self.dest.exists())

    def test_missing_path_is_refused(self):
        result = self.install(self.directory / "no-such-download.zip")
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(self.dest.exists())

    def test_default_destination_is_the_folder_the_build_reads(self):
        # The build finds the SDK at a fixed folder in the repository, named in
        # cmake/AssisiSteamworks.cmake; the script's default must be that folder.
        result = subprocess.run([sys.executable, "-I", str(SCRIPT), "--print-dest"], capture_output=True,
                                text=True, check=False, stdin=subprocess.DEVNULL)
        self.assertEqual(result.returncode, 0, result.stderr)
        default = Path(result.stdout.strip())
        self.assertEqual(default, REPO / "steamworks")
        cmake = (REPO / "cmake" / "AssisiSteamworks.cmake").read_text()
        self.assertIn('"${CMAKE_SOURCE_DIR}/steamworks"', cmake)


if __name__ == "__main__":
    unittest.main()
