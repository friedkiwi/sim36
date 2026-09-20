#!/usr/bin/env python3
"""Build deterministic, non-vendored SSP library media for FUNLIB STARTREK."""

import argparse
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


PINNED_COMMIT = "1d0d2ea221cdb7b22d611d8cef474ebd518fb70f"
INPUTS = {
    "STREK.RPG36": "5b0da3dcd43d667c2b840494ff08dcd336fa07458c796e5067779992f4187aa6",
    "STREKFM.DSPF36": "6f976e4cef7879c75aba2c385e9a08ee7369910f8a065f00e01c45c0b2f14a4f",
    "STREK.OCL36": "5e0f8a6213a45bd5c5679897eaf102ea0d07b1e75f561c8bca08ec5739ecdd80",
    "TREKLOAD.S36PROC": "555980fd8f1978f09fda92d91d7c0fee58cba525c6cce05d1702f7023e4fec9a",
}
MEMBERS = (
    ("P", "STREK", 120, "STREK.OCL36"),
    ("S", "STREK", 96, "STREK.RPG36"),
    ("S", "STREKFM", 80, "STREKFM.DSPF36"),
)
CARD_LENGTH = 120
PHYSICAL_RECORD_LENGTH = 256
EBCDIC_SPACE = b"\x40"
EBCDIC_FLAG = "S".encode("cp037")


class BuildError(Exception):
    pass


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def locate_sources(value):
    root = Path(value).resolve()
    source = root / "startrek" if (root / "startrek").is_dir() else root
    if not source.is_dir():
        raise BuildError("FUNLIB_DIR does not contain startrek sources: %s" % root)
    try:
        commit = subprocess.run(["git", "-C", str(root), "rev-parse", "HEAD"],
                                text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                check=True).stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        # A direct .../FUNLIB/startrek path has its repository one level up.
        try:
            commit = subprocess.run(["git", "-C", str(source.parent), "rev-parse", "HEAD"],
                                    text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                    check=True).stdout.strip()
        except (OSError, subprocess.CalledProcessError) as exc:
            raise BuildError("FUNLIB_DIR must be a Git checkout so its pinned commit can be verified") from exc
    if commit != PINNED_COMMIT:
        raise BuildError("FUNLIB commit is %s, expected %s" % (commit, PINNED_COMMIT))
    for name, expected in INPUTS.items():
        path = source / name
        try:
            actual = sha256(path.read_bytes())
        except OSError as exc:
            raise BuildError("%s: %s" % (path, exc.strerror or exc)) from exc
        if actual != expected:
            raise BuildError("%s SHA-256 is %s, expected %s" % (name, actual, expected))
    return source


def text_lines(path):
    try:
        text = path.read_bytes().decode("utf-8", errors="strict")
    except (OSError, UnicodeDecodeError) as exc:
        raise BuildError("%s is not strict UTF-8: %s" % (path, exc)) from exc
    return text.splitlines()


def member_lines(source, name, record_length):
    lines = text_lines(source / name)
    warnings = []
    if name == "STREKFM.DSPF36" and lines and lines[-1] == "// CEND":
        lines = lines[:-1]
    for number, line in enumerate(lines, 1):
        try:
            encoded = line.encode("cp037", errors="strict")
        except UnicodeEncodeError as exc:
            raise BuildError("%s line %d is not representable in CP037: %s" %
                             (name, number, exc)) from exc
        if len(encoded) > CARD_LENGTH:
            raise BuildError("%s line %d is %d bytes, exceeds reader card length %d" %
                             (name, number, len(encoded), CARD_LENGTH))
        if len(encoded) > record_length:
            warnings.append("%s line %d is %d bytes for RECL %d; preserved in the %d-byte reader card" %
                            (name, number, len(encoded), record_length, CARD_LENGTH))
    return lines, warnings


def reader_cards(source):
    cards = []
    warnings = []
    for member_type, member_name, record_length, filename in MEMBERS:
        cards.append("// COPY LIBRARY-%s,NAME-%s,RECL-%03d" %
                     (member_type, member_name, record_length))
        lines, member_warnings = member_lines(source, filename, record_length)
        cards.extend(lines)
        warnings.extend(member_warnings)
        cards.append("// CEND")

    historical = text_lines(source / "TREKLOAD.S36PROC")
    historical_payload = [line.replace(",FROM-READER,TO-FUNLIB", "")
                          if line.startswith("// COPY LIBRARY-") else line
                          for line in historical[2:]]
    if historical[:2] != ["// LOAD $MAINT", "// RUN"] or historical_payload != cards:
        raise BuildError("constructed library stream does not match the pinned TREKLOAD payload")
    return cards, warnings


def encode_stream(cards):
    records = []
    for number, card in enumerate(cards, 1):
        encoded = card.encode("cp037", errors="strict")
        if len(encoded) > CARD_LENGTH:
            raise BuildError("reader card %d is %d bytes, exceeds %d" %
                             (number, len(encoded), CARD_LENGTH))
        records.append(encoded.ljust(CARD_LENGTH, EBCDIC_SPACE) +
                       EBCDIC_FLAG * (PHYSICAL_RECORD_LENGTH - CARD_LENGTH))
    return b"".join(records)


def run_tool(tool, *arguments):
    result = subprocess.run([sys.executable, str(tool), *map(str, arguments)],
                            text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if result.returncode:
        raise BuildError("tape-folder.py %s failed:\n%s" %
                         (" ".join(map(str, arguments)), result.stderr.rstrip()))
    return result.stdout.strip()


def media_digest(folder):
    digest = hashlib.sha256()
    for path in sorted(Path(folder).iterdir(), key=lambda item: item.name):
        if path.is_file():
            digest.update(path.name.encode("utf-8") + b"\0")
            digest.update(path.read_bytes())
    return digest.hexdigest()


def build(args):
    source = locate_sources(args.funlib_dir)
    cards, warnings = reader_cards(source)
    stream = encode_stream(cards)
    root = Path(__file__).resolve().parents[1]
    tool = root / "tools" / "tape-folder.py"
    temporary = Path(tempfile.mkdtemp(prefix="sim36-startrek-media-"))
    try:
        stream_path = temporary / "librfile.bin"
        stream_path.write_bytes(stream)
        blank = temporary / "blank"
        work = temporary / "work"
        run_tool(tool, "init", blank, "--volume-id", args.volume_id,
                 "--owner", args.owner)
        run_tool(tool, "unpack", blank, work)
        run_tool(tool, "add-s36-library", work, stream_path,
                 "--data-set-id", args.data_set_id,
                 "--block-length", "4096", "--record-length", "256",
                 "--creation-date", args.creation_date,
                 "--expiration-date", args.expiration_date)
        pack_args = ["pack", work, args.output]
        if args.force:
            pack_args.append("--force")
        run_tool(tool, *pack_args)
        verified = run_tool(tool, "verify", args.output, "--reject-orphans")
    finally:
        shutil.rmtree(temporary)
    print("FUNLIB %s" % PINNED_COMMIT)
    for warning in warnings:
        print("WARNING: " + warning)
    print("reader records: %d; bytes: %d; SHA-256: %s" %
          (len(cards), len(stream), sha256(stream)))
    print(verified)
    print("media tree SHA-256: %s" % media_digest(args.output))


def parser():
    result = argparse.ArgumentParser(description=__doc__)
    result.add_argument("output")
    result.add_argument("--funlib-dir", default=os.environ.get("FUNLIB_DIR"),
                        help="pinned FUNLIB checkout (default: FUNLIB_DIR)")
    result.add_argument("--volume-id", default="STREK1")
    result.add_argument("--owner", default="SIM36")
    result.add_argument("--data-set-id", default="DISCFILE")
    result.add_argument("--creation-date", default="26001", metavar="YYDDD")
    result.add_argument("--expiration-date", default="99365", metavar="YYDDD")
    result.add_argument("--force", action="store_true")
    return result


def main():
    try:
        args = parser().parse_args()
        if not args.funlib_dir:
            raise BuildError("set FUNLIB_DIR or pass --funlib-dir; sources are never vendored or downloaded implicitly")
        build(args)
        return 0
    except BuildError as exc:
        print("build-startrek-tape: error: %s" % exc, file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
