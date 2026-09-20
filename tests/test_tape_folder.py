#!/usr/bin/env python3
"""Black-box tests for tools/tape-folder.py; no guest volume is required."""

import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
TOOL = ROOT / "tools" / "tape-folder.py"


def vol1(volume="TEST01", owner="OWNER"):
    record = bytearray([0x40] * 80)
    record[0:4] = "VOL1".encode("cp037")
    record[4:10] = volume.encode("cp037").ljust(6, b"\x40")
    record[10:11] = b"\x40"
    record[37:51] = owner.encode("cp037").ljust(14, b"\x40")
    return bytes(record)


def digest(data):
    return hashlib.sha256(data).hexdigest()


class TapeFolderTests(unittest.TestCase):
    def setUp(self):
        self.temp = Path(tempfile.mkdtemp(prefix="sim36-tape-tool-test-"))

    def tearDown(self):
        shutil.rmtree(self.temp)

    def run_tool(self, *arguments, ok=True):
        result = subprocess.run([sys.executable, str(TOOL), *map(str, arguments)],
                                cwd=ROOT, text=True, stdout=subprocess.PIPE,
                                stderr=subprocess.PIPE)
        if ok and result.returncode != 0:
            self.fail("tool failed (%s):\nstdout:\n%s\nstderr:\n%s" %
                      (" ".join(map(str, arguments)), result.stdout, result.stderr))
        if not ok and result.returncode == 0:
            self.fail("tool unexpectedly accepted: " + " ".join(map(str, arguments)))
        return result

    def write_workspace(self, name, files):
        work = self.temp / name
        work.mkdir()
        entries = []
        for sequence, item in enumerate(files, 1):
            directory = work / "files" / ("%04d" % sequence)
            directory.mkdir(parents=True)
            blocks = []
            for number, data in enumerate(item["blocks"], 1):
                relative = "files/%04d/block-%06d.bin" % (sequence, number)
                (work / relative).write_bytes(data)
                blocks.append({"path": relative, "length": len(data),
                               "sha256": digest(data)})
            entries.append({"sequence": sequence,
                            "kind": item.get("kind", "data"),
                            "recordFormat": item.get("recordFormat", "U"),
                            "recordLength": item.get("recordLength", 0),
                            "blocks": blocks})
        manifest = {"format": "s36-tape-work", "formatVersion": 1,
                    "volume": {"volumeId": "TEST01", "accessSecurity": " ",
                               "ownerId": "OWNER", "labeled": True},
                    "files": entries}
        (work / "tape.json").write_text(json.dumps(manifest, indent=2) + "\n",
                                         encoding="utf-8")
        return work

    def tree_bytes(self, folder):
        return {path.relative_to(folder).as_posix(): path.read_bytes()
                for path in folder.rglob("*") if path.is_file()}

    def logical_stream(self, tape):
        manifest = json.loads((tape / "manifest.json").read_text(encoding="utf-8"))
        stream = []
        for entry in manifest["files"]:
            lengths = entry.get("blockLengths")
            if lengths is None:
                lengths = [entry["blockLength"]] * entry["blockCount"]
            data = (tape / entry["blob"]).read_bytes()
            offset = 0
            for length in lengths:
                stream.append(("block", data[offset:offset + length]))
                offset += length
            stream.append(("mark", b""))
        return stream

    def test_empty_labeled_media_and_read_only_inspection(self):
        tape = self.temp / "blank"
        self.run_tool("init", tape, "--volume-id", "TEST01", "--owner", "OWNER")
        before = {path.name: (path.stat().st_mtime_ns, path.read_bytes())
                  for path in tape.iterdir()}
        self.assertIn("OK: 2 tape file(s), 1 block(s), 80 byte(s)",
                      self.run_tool("verify", tape).stdout)
        self.assertEqual([("block", vol1()), ("mark", b""), ("mark", b"")],
                         self.logical_stream(tape))
        self.assertIn("TEST01", self.run_tool("list", tape).stdout)
        decoded = json.loads(self.run_tool("labels", tape).stdout)
        self.assertEqual("TEST01", decoded["labelGroups"][0]["labels"]["vol1"]["volumeId"])
        after = {path.name: (path.stat().st_mtime_ns, path.read_bytes())
                 for path in tape.iterdir()}
        self.assertEqual(before, after)

    def test_fixed_variable_multiple_files_marks_and_determinism(self):
        work = self.write_workspace("editable", [
            {"blocks": [vol1()], "kind": "label", "recordFormat": "F", "recordLength": 80},
            {"blocks": [b"AAA", b"BBB"], "recordFormat": "F", "recordLength": 3},
            {"blocks": [b"x", b"variable", b"zz"]},
            {"blocks": []},
            {"blocks": []},
        ])
        first = self.temp / "first"
        second = self.temp / "second"
        self.run_tool("pack", work, first)
        self.run_tool("verify", first)
        manifest = json.loads((first / "manifest.json").read_text(encoding="utf-8"))
        self.assertEqual(3, manifest["files"][1]["blockLength"])
        self.assertEqual([1, 8, 2], manifest["files"][2]["blockLengths"])
        self.assertEqual(0, manifest["files"][3]["blockCount"])
        self.assertEqual(0, manifest["files"][4]["blockCount"])

        unpacked = self.temp / "unpacked"
        self.run_tool("unpack", first, unpacked)
        self.run_tool("pack", unpacked, second)
        self.assertEqual(self.logical_stream(first), self.logical_stream(second))
        self.assertEqual(self.tree_bytes(first), self.tree_bytes(second))

        third = self.temp / "third"
        self.run_tool("pack", unpacked, third)
        self.assertEqual(self.tree_bytes(second), self.tree_bytes(third))

        extracted = self.temp / "extracted"
        self.run_tool("extract", first, extracted, "--all")
        self.assertEqual(b"x", (extracted / "0003" / "block-000001.bin").read_bytes())
        self.assertEqual(b"xvariablezz", (extracted / "0003" / "joined.bin").read_bytes())

    def test_import_text_cp037_and_overlong_rejection(self):
        tape = self.temp / "blank"
        work = self.temp / "work"
        self.run_tool("init", tape, "--volume-id", "TEST01", "--owner", "OWNER")
        self.run_tool("unpack", tape, work)
        source = self.temp / "source.txt"
        source.write_text("ABC\nD\n", encoding="utf-8", newline="\n")
        self.run_tool("import-text", work, source, "--record-length", "4",
                      "--records-per-block", "2")
        packed = self.temp / "text-tape"
        self.run_tool("pack", work, packed)
        manifest = json.loads((packed / "manifest.json").read_text(encoding="utf-8"))
        self.assertEqual(4, manifest["files"][2]["recordLength"])
        self.assertEqual(8, manifest["files"][2]["blockLength"])
        self.assertEqual("ABC D   ", (packed / "0003.dat").read_bytes().decode("cp037"))

        too_long = self.temp / "too-long.txt"
        too_long.write_text("12345\n", encoding="utf-8")
        result = self.run_tool("import-text", work, too_long,
                               "--record-length", "4", ok=False)
        self.assertIn("longer than record length", result.stderr)

    def test_native_s36_library_label_authoring(self):
        tape = self.temp / "blank-library"
        work = self.temp / "library-work"
        stream = self.temp / "library.bin"
        self.run_tool("init", tape, "--volume-id", "TEST01", "--owner", "OWNER")
        self.run_tool("unpack", tape, work)
        stream.write_bytes(b"A" * 256 + b"B" * 256 + b"C" * 256)
        self.run_tool("add-s36-library", work, stream,
                      "--data-set-id", "DISCFILE",
                      "--block-length", "512", "--record-length", "256",
                      "--creation-date", "26263", "--expiration-date", "26264")
        first = self.temp / "library-first"
        second = self.temp / "library-second"
        self.run_tool("pack", work, first)
        self.run_tool("pack", work, second)
        self.run_tool("verify", first)
        self.assertEqual(self.tree_bytes(first), self.tree_bytes(second))

        manifest = json.loads((first / "manifest.json").read_text(encoding="utf-8"))
        self.assertEqual(5, len(manifest["files"]))
        self.assertEqual([512, 256], manifest["files"][2]["blockLengths"])
        self.assertEqual("DISCFILE", manifest["files"][1]["labels"]["hdr1"]["dataSetId"])
        headers = (first / "0002.dat").read_bytes()
        trailers = (first / "0004.dat").read_bytes()
        self.assertEqual(320, len(headers))
        self.assertEqual(b"HDR1", headers[:4].decode("cp037").encode("ascii"))
        self.assertEqual("HDR2F0051200256 0FROMLIBR/$MAINT      B",
                         headers[80:119].decode("cp037"))
        self.assertEqual("UHL1LIBRFILER&" + "S" * 66,
                         headers[160:240].decode("cp037"))
        self.assertEqual("EOF1", trailers[:4].decode("cp037"))
        self.assertEqual(headers[4:80], trailers[4:80])
        self.assertEqual("UTL2", trailers[240:244].decode("cp037"))

        bad = self.temp / "bad-library.bin"
        bad.write_bytes(b"not-a-record")
        fresh_tape = self.temp / "fresh"
        fresh_work = self.temp / "fresh-work"
        self.run_tool("init", fresh_tape, "--volume-id", "TEST01")
        self.run_tool("unpack", fresh_tape, fresh_work)
        result = self.run_tool("add-s36-library", fresh_work, bad,
                               "--data-set-id", "DISCFILE",
                               "--creation-date", "26263",
                               "--expiration-date", "26264", ok=False)
        self.assertIn("not a multiple", result.stderr)

    def test_corruption_is_rejected_with_diagnostics(self):
        original = self.temp / "original"
        self.run_tool("init", original, "--volume-id", "TEST01", "--owner", "OWNER")

        def corrupt(name, mutate=None, blob=None):
            target = self.temp / name
            shutil.copytree(original, target)
            if mutate is not None:
                path = target / "manifest.json"
                value = json.loads(path.read_text(encoding="utf-8"))
                mutate(value)
                path.write_text(json.dumps(value) + "\n", encoding="utf-8")
            if blob is not None:
                blob(target)
            return self.run_tool("verify", target, ok=False)

        self.assertIn("manifest format", corrupt(
            "format", lambda value: value.update(format="wrong")).stderr)
        self.assertIn("formatVersion", corrupt(
            "version", lambda value: value.update(formatVersion=99)).stderr)
        self.assertIn("safe relative path", corrupt(
            "traversal", lambda value: value["files"][0].update(blob="../outside.dat")).stderr)
        self.assertIn("sequence must be 1", corrupt(
            "sequence", lambda value: value["files"][0].update(sequence=2)).stderr)
        self.assertIn("must be at most", corrupt(
            "length", lambda value: value["files"][0].update(blockLength=40000)).stderr)
        self.assertIn("recordFormat", corrupt(
            "record-format", lambda value: value["files"][0].update(recordFormat="X")).stderr)
        self.assertIn("blob size", corrupt(
            "truncated", blob=lambda folder: (folder / "0001.dat").write_bytes(b"short")).stderr)
        self.assertIn("labels does not match", corrupt(
            "labels", lambda value: value["files"][0]["labels"]["vol1"].update(ownerId="OTHER")).stderr)
        self.assertIn("is missing", corrupt(
            "missing", blob=lambda folder: (folder / "0001.dat").unlink()).stderr)

        malformed = self.temp / "malformed"
        shutil.copytree(original, malformed)
        (malformed / "manifest.json").write_text("{not json\n", encoding="utf-8")
        self.assertIn("invalid JSON", self.run_tool("verify", malformed, ok=False).stderr)

        orphan = self.temp / "orphan"
        shutil.copytree(original, orphan)
        (orphan / "9999.dat").write_bytes(b"orphan")
        self.assertIn("warning: orphaned blob", self.run_tool("verify", orphan).stderr)
        rejected = self.run_tool("verify", orphan, "--reject-orphans", ok=False)
        self.assertIn("orphaned blob", rejected.stderr)


if __name__ == "__main__":
    unittest.main()
