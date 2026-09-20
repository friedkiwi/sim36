#!/usr/bin/env python3
"""Inspect and author SIM/36 folder tapes without private guest media.

The on-media authority is the ordered block stream in the blobs.  Decoded
labels in manifest.json are checked mirrors, never a substitute for bytes.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import shutil
import sys
import tempfile
import uuid


FORMAT = "s36-folder-tape"
VERSION = 1
WORK_FORMAT = "s36-tape-work"
WORK_VERSION = 1
MANIFEST = "manifest.json"
WORK_MANIFEST = "tape.json"
MAX_BLOCK = 0x7FFF
LABEL_LENGTH = 80
EBCDIC_SPACE = 0x40
KNOWN_LABELS = {"VOL1", "HDR1", "HDR2", "EOF1", "EOF2", "EOV1", "EOV2"}


class TapeError(Exception):
    pass


def fail(message):
    raise TapeError(message)


def read_json(path):
    try:
        with path.open("r", encoding="utf-8") as source:
            return json.load(source)
    except OSError as exc:
        fail("%s: %s" % (path, exc.strerror or exc))
    except (UnicodeError, json.JSONDecodeError) as exc:
        fail("%s: invalid JSON: %s" % (path, exc))


def write_json(path, value):
    data = json.dumps(value, ensure_ascii=False, indent=2, separators=(",", ": ")) + "\n"
    with path.open("x", encoding="utf-8", newline="\n") as target:
        target.write(data)


def integer(value, where, minimum=None, maximum=None):
    if isinstance(value, bool) or not isinstance(value, int):
        fail("%s must be an integer" % where)
    if minimum is not None and value < minimum:
        fail("%s must be at least %d" % (where, minimum))
    if maximum is not None and value > maximum:
        fail("%s must be at most %d" % (where, maximum))
    return value


def safe_relative(value, where):
    if not isinstance(value, str) or not value:
        fail("%s must be a non-empty relative path" % where)
    if "\\" in value:
        fail("%s uses a backslash; paths must use portable '/' separators" % where)
    path = PurePosixPath(value)
    if path.is_absolute() or any(part in ("", ".", "..") for part in path.parts):
        fail("%s is not a safe relative path: %r" % (where, value))
    return path


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def trim_ebcdic(data):
    return data.decode("cp037", errors="replace").rstrip(" \x00")


def field(record, offset, length):
    return trim_ebcdic(record[offset:offset + length])


def label_id(record):
    return field(record, 0, 4) if len(record) == LABEL_LENGTH else ""


def decode_label(record):
    ident = label_id(record)
    if ident == "VOL1":
        return ident, {
            "volumeId": field(record, 4, 6),
            "accessSecurity": field(record, 10, 1),
            "ownerId": field(record, 37, 14),
        }
    if ident in ("HDR2", "EOF2", "EOV2"):
        return ident, {
            "labelId": ident,
            "recordFormat": field(record, 4, 1),
            "blockLength": field(record, 5, 5),
        }
    if ident in KNOWN_LABELS:
        return ident, {
            "labelId": ident,
            "dataSetId": field(record, 4, 17),
            "dataSetSequenceNumber": field(record, 31, 4),
            "blockCount": field(record, 54, 6),
        }
    return "", None


def decode_label_group(blocks):
    if not blocks or label_id(blocks[0]) not in KNOWN_LABELS:
        return {}
    result = {}
    for record in blocks:
        ident, decoded = decode_label(record)
        if ident:
            result[ident.lower()] = decoded
    return result


def put_ebcdic(record, offset, text, length):
    encoded = text[:length].encode("cp037", errors="strict")
    record[offset:offset + length] = encoded.ljust(length, bytes([EBCDIC_SPACE]))


def normalise_volume_id(value):
    value = value.strip() or "S36VOL"
    try:
        value.encode("cp037")
    except UnicodeEncodeError as exc:
        fail("volume ID is not representable in EBCDIC CP037: %s" % exc)
    return value[:6]


def render_vol1(volume_id, owner_id, access=" "):
    record = bytearray([EBCDIC_SPACE] * LABEL_LENGTH)
    put_ebcdic(record, 0, "VOL1", 4)
    put_ebcdic(record, 4, normalise_volume_id(volume_id), 6)
    put_ebcdic(record, 10, access or " ", 1)
    put_ebcdic(record, 37, owner_id, 14)
    return bytes(record)


def fixed_digits(value, name, length):
    if not isinstance(value, str) or len(value) != length or not value.isascii() or not value.isdigit():
        fail("%s must be exactly %d ASCII digits" % (name, length))
    return value


def render_s36_library_labels(data_set_id, block_length, record_length,
                              creation_date, expiration_date, producer, system_code):
    """Render the four-label group emitted by native SSP FROMLIBR.

    Field positions and constants are evidence-backed by an overlay-backed
    guest export; callers must provide dates explicitly so media construction
    never depends on the host clock.
    """
    try:
        encoded_id = data_set_id.encode("cp037", errors="strict")
        producer.encode("cp037", errors="strict")
        system_code.encode("cp037", errors="strict")
    except UnicodeEncodeError as exc:
        fail("S/36 label text is not representable in EBCDIC CP037: %s" % exc)
    if not data_set_id or len(encoded_id) > 17:
        fail("data set ID must be 1..17 CP037 bytes")
    if len(producer.encode("cp037")) > 15:
        fail("producer must be at most 15 CP037 bytes")
    if len(system_code.encode("cp037")) > 13:
        fail("system code must be at most 13 CP037 bytes")
    fixed_digits(creation_date, "creation date", 5)
    fixed_digits(expiration_date, "expiration date", 5)
    if not 1 <= block_length <= MAX_BLOCK:
        fail("block length must be in 1..%d" % MAX_BLOCK)
    if not 1 <= record_length <= block_length:
        fail("record length must be in 1..block length")

    hdr1 = bytearray([EBCDIC_SPACE] * LABEL_LENGTH)
    put_ebcdic(hdr1, 0, "HDR1", 4)
    put_ebcdic(hdr1, 4, data_set_id, 17)
    put_ebcdic(hdr1, 27, "0001", 4)
    put_ebcdic(hdr1, 35, "000100", 6)
    put_ebcdic(hdr1, 41, "0" + creation_date, 6)
    put_ebcdic(hdr1, 47, "0" + expiration_date, 6)
    put_ebcdic(hdr1, 53, "0000000", 7)
    put_ebcdic(hdr1, 60, system_code, 13)

    hdr2 = bytearray([EBCDIC_SPACE] * LABEL_LENGTH)
    put_ebcdic(hdr2, 0, "HDR2", 4)
    put_ebcdic(hdr2, 4, "F", 1)
    put_ebcdic(hdr2, 5, "%05d" % block_length, 5)
    put_ebcdic(hdr2, 10, "%05d" % record_length, 5)
    put_ebcdic(hdr2, 16, "0", 1)
    put_ebcdic(hdr2, 17, producer, 15)
    put_ebcdic(hdr2, 38, "B", 1)

    uhl1 = bytearray("UHL1LIBRFILER&".encode("cp037") + "S".encode("cp037") * 66)
    uhl2 = bytearray("UHL2".encode("cp037") + "S".encode("cp037") * 76)
    headers = [bytes(hdr1), bytes(hdr2), bytes(uhl1), bytes(uhl2)]
    trailers = [bytearray(record) for record in headers]
    for record, ident in zip(trailers, ("EOF1", "EOF2", "UTL1", "UTL2")):
        put_ebcdic(record, 0, ident, 4)
    return headers, [bytes(record) for record in trailers]


def block_lengths(entry, where):
    count = integer(entry.get("blockCount"), where + ".blockCount", 0)
    if "blockLengths" in entry:
        values = entry["blockLengths"]
        if not isinstance(values, list):
            fail(where + ".blockLengths must be an array")
        if len(values) != count:
            fail("%s.blockCount is %d but blockLengths has %d entries" %
                 (where, count, len(values)))
        return [integer(value, "%s.blockLengths[%d]" % (where, index), 1, MAX_BLOCK)
                for index, value in enumerate(values)]
    length = integer(entry.get("blockLength"), where + ".blockLength", 0, MAX_BLOCK)
    if count and length == 0:
        fail(where + ".blockLength must be positive when blockCount is nonzero")
    return [length] * count


def validate_volume(value):
    if not isinstance(value, dict):
        fail("volume must be an object")
    for name in ("volumeId", "accessSecurity", "ownerId"):
        if name in value and not isinstance(value[name], str):
            fail("volume.%s must be a string" % name)
    if "labeled" in value and not isinstance(value["labeled"], bool):
        fail("volume.labeled must be a boolean")
    return value


def validate_metadata(entry, where):
    kind = entry.get("kind", "data")
    if not isinstance(kind, str) or not kind:
        fail(where + ".kind must be a non-empty string")
    record_format = entry.get("recordFormat", "U")
    if record_format not in ("F", "V", "U"):
        fail(where + ".recordFormat must be F, V, or U")
    integer(entry.get("recordLength", 0), where + ".recordLength", 0, MAX_BLOCK)


def reject_symlink_path(root, relative, where):
    current = root
    for part in relative.parts:
        current = current / part
        if current.is_symlink():
            fail("%s may not refer through a symbolic link: %s" % (where, relative))


def load_tape(folder, reject_orphans=False, include_data=False):
    folder = Path(folder)
    if not folder.is_dir():
        fail("%s is not a tape folder" % folder)
    root = read_json(folder / MANIFEST)
    if not isinstance(root, dict):
        fail("manifest root must be an object")
    if root.get("format") != FORMAT:
        fail("manifest format is %r, expected %r" % (root.get("format"), FORMAT))
    if root.get("formatVersion") != VERSION:
        fail("manifest formatVersion is %r, expected %d" %
             (root.get("formatVersion"), VERSION))
    volume = validate_volume(root.get("volume", {}))
    entries = root.get("files")
    if not isinstance(entries, list):
        fail("files must be an array")

    used = set()
    files = []
    warnings = []
    for index, entry in enumerate(entries, 1):
        where = "files[%d]" % (index - 1)
        if not isinstance(entry, dict):
            fail(where + " must be an object")
        if integer(entry.get("sequence"), where + ".sequence", 1) != index:
            fail("%s.sequence must be %d" % (where, index))
        relative = safe_relative(entry.get("blob"), where + ".blob")
        if relative.as_posix() in used:
            fail("%s.blob duplicates %s" % (where, relative))
        used.add(relative.as_posix())
        validate_metadata(entry, where)
        reject_symlink_path(folder, relative, where + ".blob")
        path = folder.joinpath(*relative.parts)
        if not path.is_file():
            fail("%s.blob is missing: %s" % (where, relative))
        lengths = block_lengths(entry, where)
        expected = sum(lengths)
        actual = path.stat().st_size
        if actual != expected:
            fail("%s: blob size is %d bytes, manifest declares %d" %
                 (relative, actual, expected))
        data = path.read_bytes()
        blocks = []
        offset = 0
        for length in lengths:
            blocks.append(data[offset:offset + length])
            offset += length
        decoded = decode_label_group(blocks)
        labels = entry.get("labels", {})
        if not isinstance(labels, dict):
            fail(where + ".labels must be an object")
        if labels != decoded:
            fail("%s.labels does not match the EBCDIC label records in %s" %
                 (where, relative))
        files.append({"entry": entry, "lengths": lengths,
                      "blocks": blocks if include_data else None,
                      "byteCount": expected, "decodedLabels": decoded})

    actual_blobs = {path.relative_to(folder).as_posix()
                    for path in folder.rglob("*.dat") if path.is_file()}
    orphans = sorted(actual_blobs - used)
    if orphans:
        message = "orphaned blob(s): " + ", ".join(orphans)
        if reject_orphans:
            fail(message)
        warnings.append(message)

    if volume.get("labeled", True):
        if not files or "vol1" not in files[0]["decodedLabels"]:
            fail("labeled volume does not begin with a VOL1 label block")
        vol1 = files[0]["decodedLabels"]["vol1"]
        if volume.get("volumeId", "") != vol1["volumeId"]:
            fail("volume.volumeId does not match the VOL1 bytes")
        if volume.get("ownerId", "") != vol1["ownerId"]:
            fail("volume.ownerId does not match the VOL1 bytes")
        if volume.get("accessSecurity", "").rstrip() != vol1["accessSecurity"]:
            fail("volume.accessSecurity does not match the VOL1 bytes")
    return root, files, warnings


def canonical_entry(sequence, blocks, metadata=None, blob=None):
    metadata = metadata or {}
    lengths = [len(block) for block in blocks]
    entry = {
        "sequence": sequence,
        "kind": metadata.get("kind", "data"),
        "blob": blob or "%04d.dat" % sequence,
    }
    if not lengths or all(length == lengths[0] for length in lengths):
        entry["blockLength"] = lengths[0] if lengths else 0
        entry["blockCount"] = len(lengths)
    else:
        entry["blockLengths"] = lengths
        entry["blockCount"] = len(lengths)
    entry["recordFormat"] = metadata.get("recordFormat", "U")
    entry["recordLength"] = integer(metadata.get("recordLength", 0),
                                    "recordLength", 0, MAX_BLOCK)
    labels = decode_label_group(blocks)
    if labels:
        entry["labels"] = labels
        entry["kind"] = "label"
    return entry


def replace_directory(target, builder, force=False):
    target = Path(target)
    parent = target.parent
    parent.mkdir(parents=True, exist_ok=True)
    if target.exists() and not force:
        fail("%s already exists (use --force to replace it)" % target)
    temporary = Path(tempfile.mkdtemp(prefix=".%s.tmp-" % target.name, dir=str(parent)))
    backup = None
    try:
        builder(temporary)
        if target.exists():
            backup = parent / (".%s.old-%s" % (target.name, uuid.uuid4().hex))
            os.replace(str(target), str(backup))
        try:
            os.replace(str(temporary), str(target))
        except Exception:
            if backup is not None:
                os.replace(str(backup), str(target))
                backup = None
            raise
        if backup is not None:
            shutil.rmtree(backup)
    finally:
        if temporary.exists():
            shutil.rmtree(temporary)


def write_tape(target, volume, tape_files, force=False):
    def build(folder):
        entries = []
        for sequence, item in enumerate(tape_files, 1):
            blocks = item["blocks"]
            for number, block in enumerate(blocks, 1):
                if not block or len(block) > MAX_BLOCK:
                    fail("file %d block %d length %d is outside 1..%d" %
                         (sequence, number, len(block), MAX_BLOCK))
            name = "%04d.dat" % sequence
            with (folder / name).open("xb") as blob:
                for block in blocks:
                    blob.write(block)
            entries.append(canonical_entry(sequence, blocks, item.get("metadata"), name))
        manifest = {"format": FORMAT, "formatVersion": VERSION,
                    "volume": volume, "files": entries}
        write_json(folder / MANIFEST, manifest)
        load_tape(folder, include_data=False)
    replace_directory(target, build, force=force)


def command_init(args):
    volume_id = normalise_volume_id(args.volume_id)
    try:
        args.owner.encode("cp037")
    except UnicodeEncodeError as exc:
        fail("owner is not representable in EBCDIC CP037: %s" % exc)
    vol1 = render_vol1(volume_id, args.owner, args.access)
    volume = {"volumeId": volume_id,
              "accessSecurity": (args.access or " ")[:1],
              "ownerId": args.owner[:14], "labeled": True}
    write_tape(args.tape_dir, volume,
               [{"blocks": [vol1], "metadata": {"kind": "label",
                                                  "recordFormat": "F",
                                                  "recordLength": LABEL_LENGTH}},
                {"blocks": [], "metadata": {"kind": "data",
                                                "recordFormat": "U",
                                                "recordLength": 0}}],
               force=args.force)
    print("initialised %s as volume %s" % (args.tape_dir, volume_id))


def command_verify(args):
    _, files, warnings = load_tape(args.tape_dir,
                                   reject_orphans=args.reject_orphans,
                                   include_data=False)
    for warning in warnings:
        print("warning: " + warning, file=sys.stderr)
    print("OK: %d tape file(s), %d block(s), %d byte(s)" %
          (len(files), sum(len(item["lengths"]) for item in files),
           sum(item["byteCount"] for item in files)))


def command_list(args):
    root, files, warnings = load_tape(args.tape_dir, include_data=False)
    volume = root["volume"]
    print("volume %s  owner %s  labeled %s" %
          (volume.get("volumeId", ""), volume.get("ownerId", ""),
           str(volume.get("labeled", True)).lower()))
    print("seq kind   fmt record blocks bytes block-lengths blob")
    for item in files:
        entry = item["entry"]
        lengths = item["lengths"]
        form = str(lengths[0]) if lengths and all(x == lengths[0] for x in lengths) else (
            "-" if not lengths else ",".join(str(x) for x in lengths))
        print("%3d %-6s %-3s %6d %6d %5d %-13s %s" %
              (entry["sequence"], entry.get("kind", "data"),
               entry.get("recordFormat", "U"), entry.get("recordLength", 0),
               len(lengths), item["byteCount"], form, entry["blob"]))
    for warning in warnings:
        print("warning: " + warning, file=sys.stderr)


def work_file(sequence, number):
    return "files/%04d/block-%06d.bin" % (sequence, number)


def command_unpack(args):
    root, files, _ = load_tape(args.tape_dir, include_data=True)
    def build(folder):
        work_files = []
        for sequence, item in enumerate(files, 1):
            blocks = []
            block_dir = folder / "files" / ("%04d" % sequence)
            block_dir.mkdir(parents=True)
            for number, data in enumerate(item["blocks"], 1):
                relative = work_file(sequence, number)
                path = folder.joinpath(*PurePosixPath(relative).parts)
                with path.open("xb") as target:
                    target.write(data)
                blocks.append({"path": relative, "length": len(data),
                               "sha256": sha256(data)})
            entry = item["entry"]
            work_files.append({"sequence": sequence,
                               "kind": entry.get("kind", "data"),
                               "recordFormat": entry.get("recordFormat", "U"),
                               "recordLength": entry.get("recordLength", 0),
                               "blocks": blocks})
        work = {"format": WORK_FORMAT, "formatVersion": WORK_VERSION,
                "volume": root["volume"], "files": work_files}
        write_json(folder / WORK_MANIFEST, work)
    replace_directory(args.work_dir, build, force=args.force)
    print("unpacked %d tape file(s) into %s" % (len(files), args.work_dir))


def load_work(folder):
    folder = Path(folder)
    root = read_json(folder / WORK_MANIFEST)
    if not isinstance(root, dict) or root.get("format") != WORK_FORMAT:
        fail("%s is not a %s workspace" % (folder, WORK_FORMAT))
    if root.get("formatVersion") != WORK_VERSION:
        fail("workspace formatVersion is %r, expected %d" %
             (root.get("formatVersion"), WORK_VERSION))
    volume = validate_volume(root.get("volume", {}))
    values = root.get("files")
    if not isinstance(values, list):
        fail("workspace files must be an array")
    files = []
    used = set()
    for sequence, entry in enumerate(values, 1):
        where = "files[%d]" % (sequence - 1)
        if not isinstance(entry, dict):
            fail(where + " must be an object")
        if integer(entry.get("sequence"), where + ".sequence", 1) != sequence:
            fail("%s.sequence must be %d" % (where, sequence))
        values = entry.get("blocks")
        if not isinstance(values, list):
            fail(where + ".blocks must be an array")
        validate_metadata(entry, where)
        blocks = []
        for number, block in enumerate(values, 1):
            bwhere = "%s.blocks[%d]" % (where, number - 1)
            if not isinstance(block, dict):
                fail(bwhere + " must be an object")
            relative = safe_relative(block.get("path"), bwhere + ".path")
            if relative.as_posix() in used:
                fail(bwhere + ".path is duplicated")
            used.add(relative.as_posix())
            reject_symlink_path(folder, relative, bwhere + ".path")
            path = folder.joinpath(*relative.parts)
            if not path.is_file():
                fail("%s is missing" % path)
            data = path.read_bytes()
            declared = integer(block.get("length"), bwhere + ".length", 1, MAX_BLOCK)
            if len(data) != declared:
                fail("%s length is %d, workspace declares %d" %
                     (relative, len(data), declared))
            digest = block.get("sha256")
            if not isinstance(digest, str) or digest != sha256(data):
                fail("%s sha256 does not match its bytes" % relative)
            blocks.append(data)
        metadata = {"kind": entry.get("kind", "data"),
                    "recordFormat": entry.get("recordFormat", "U"),
                    "recordLength": integer(entry.get("recordLength", 0),
                                            where + ".recordLength", 0, MAX_BLOCK)}
        files.append({"blocks": blocks, "metadata": metadata})
    return root, volume, files


def command_pack(args):
    _, volume, files = load_work(args.work_dir)
    write_tape(args.tape_dir, volume, files, force=args.force)
    print("packed %d tape file(s) into %s" % (len(files), args.tape_dir))


def command_extract(args):
    _, files, _ = load_tape(args.tape_dir, include_data=True)
    selected = range(1, len(files) + 1) if args.all else [args.file]
    for sequence in selected:
        if sequence is None or sequence < 1 or sequence > len(files):
            fail("tape file %r does not exist" % sequence)
    def build(folder):
        index = {"format": "s36-tape-extract", "formatVersion": 1, "files": []}
        for sequence in selected:
            item = files[sequence - 1]
            target = folder / ("%04d" % sequence)
            target.mkdir(parents=True)
            blocks = []
            joined = bytearray()
            for number, data in enumerate(item["blocks"], 1):
                name = "block-%06d.bin" % number
                with (target / name).open("xb") as output:
                    output.write(data)
                blocks.append({"path": "%04d/%s" % (sequence, name),
                               "length": len(data), "sha256": sha256(data)})
                joined.extend(data)
            with (target / "joined.bin").open("xb") as output:
                output.write(joined)
            index["files"].append({"sequence": sequence, "blocks": blocks,
                                   "joined": "%04d/joined.bin" % sequence})
        write_json(folder / "extract.json", index)
    replace_directory(args.output_dir, build, force=args.force)
    print("extracted %d tape file(s) into %s" % (len(list(selected)), args.output_dir))


def command_import_text(args):
    root, _, files = load_work(args.work_dir)
    try:
        text = Path(args.text_file).read_text(encoding=args.encoding)
    except (OSError, UnicodeError, LookupError) as exc:
        fail("%s: %s" % (args.text_file, exc))
    records = []
    for number, line in enumerate(text.splitlines(), 1):
        try:
            encoded = line.encode("cp037", errors="strict")
        except UnicodeEncodeError as exc:
            fail("line %d is not representable in EBCDIC CP037: %s" % (number, exc))
        if len(encoded) > args.record_length:
            fail("line %d is %d bytes, longer than record length %d" %
                 (number, len(encoded), args.record_length))
        records.append(encoded.ljust(args.record_length, bytes([EBCDIC_SPACE])))
    blocks = [b"".join(records[index:index + args.records_per_block])
              for index in range(0, len(records), args.records_per_block)]
    if any(len(block) > MAX_BLOCK for block in blocks):
        fail("records-per-block produces a block larger than %d bytes" % MAX_BLOCK)
    files.append({"blocks": blocks,
                  "metadata": {"kind": "data", "recordFormat": "F",
                               "recordLength": args.record_length}})
    # Rewrite the workspace canonically and atomically so imported block files
    # cannot be left half-created.  Reuse a temporary tape as the normalising
    # bridge; both representations are independently verified.
    temp_tape = Path(tempfile.mkdtemp(prefix="s36-import-tape-"))
    shutil.rmtree(temp_tape)
    try:
        write_tape(temp_tape, root["volume"], files)
        unpack_args = argparse.Namespace(tape_dir=str(temp_tape),
                                         work_dir=args.work_dir, force=True)
        command_unpack(unpack_args)
    finally:
        if temp_tape.exists():
            shutil.rmtree(temp_tape)
    print("imported %d record(s) as tape file %d" % (len(records), len(files)))


def command_add_s36_library(args):
    root, _, files = load_work(args.work_dir)
    if len(files) != 2 or len(files[0]["blocks"]) != 1 or files[1]["blocks"]:
        fail("add-s36-library requires a blank labeled workspace from init/unpack")
    if label_id(files[0]["blocks"][0]) != "VOL1":
        fail("add-s36-library requires VOL1 as its first tape file")
    try:
        data = Path(args.data_file).read_bytes()
    except OSError as exc:
        fail("%s: %s" % (args.data_file, exc.strerror or exc))
    if not data:
        fail("library data stream must not be empty")
    if len(data) % args.record_length:
        fail("library data is %d bytes, not a multiple of record length %d" %
             (len(data), args.record_length))
    headers, trailers = render_s36_library_labels(
        args.data_set_id, args.block_length, args.record_length,
        args.creation_date, args.expiration_date, args.producer, args.system_code)
    blocks = [data[offset:offset + args.block_length]
              for offset in range(0, len(data), args.block_length)]
    authored = [files[0],
                {"blocks": headers, "metadata": {"kind": "label", "recordFormat": "F",
                                                    "recordLength": LABEL_LENGTH}},
                {"blocks": blocks, "metadata": {"kind": "data", "recordFormat": "U",
                                                   "recordLength": 0}},
                {"blocks": trailers, "metadata": {"kind": "label", "recordFormat": "F",
                                                     "recordLength": LABEL_LENGTH}},
                {"blocks": [], "metadata": {"kind": "data", "recordFormat": "U",
                                                "recordLength": 0}}]
    temp_tape = Path(tempfile.mkdtemp(prefix="s36-library-tape-"))
    shutil.rmtree(temp_tape)
    try:
        write_tape(temp_tape, root["volume"], authored)
        command_unpack(argparse.Namespace(tape_dir=str(temp_tape),
                                          work_dir=args.work_dir, force=True))
    finally:
        if temp_tape.exists():
            shutil.rmtree(temp_tape)
    print("added S/36 library data set %s: %d record(s), %d block(s)" %
          (args.data_set_id, len(data) // args.record_length, len(blocks)))


def command_labels(args):
    root, files, _ = load_tape(args.tape_dir, include_data=False)
    result = {"volume": root["volume"], "labelGroups": []}
    for item in files:
        if item["decodedLabels"]:
            result["labelGroups"].append({"sequence": item["entry"]["sequence"],
                                           "labels": item["decodedLabels"]})
    print(json.dumps(result, ensure_ascii=False, indent=2,
                     separators=(",", ": ")))


def parser():
    top = argparse.ArgumentParser(description=__doc__)
    sub = top.add_subparsers(dest="command", required=True)

    command = sub.add_parser("init", help="create a blank VOL1-labeled tape")
    command.add_argument("tape_dir")
    command.add_argument("--volume-id", required=True)
    command.add_argument("--owner", default="SIM36")
    command.add_argument("--access", default=" ")
    command.add_argument("--force", action="store_true")
    command.set_defaults(function=command_init)

    command = sub.add_parser("verify", help="validate manifest, blobs, blocks, and labels")
    command.add_argument("tape_dir")
    command.add_argument("--reject-orphans", action="store_true")
    command.set_defaults(function=command_verify)

    command = sub.add_parser("list", help="list volume and tape-file structure")
    command.add_argument("tape_dir")
    command.set_defaults(function=command_list)

    command = sub.add_parser("unpack", help="unpack into one file per block")
    command.add_argument("tape_dir")
    command.add_argument("work_dir")
    command.add_argument("--force", action="store_true")
    command.set_defaults(function=command_unpack)

    command = sub.add_parser("pack", help="pack an editable workspace canonically")
    command.add_argument("work_dir")
    command.add_argument("tape_dir")
    command.add_argument("--force", action="store_true")
    command.set_defaults(function=command_pack)

    command = sub.add_parser("extract", help="extract selected tape files with block boundaries")
    command.add_argument("tape_dir")
    command.add_argument("output_dir")
    select = command.add_mutually_exclusive_group(required=True)
    select.add_argument("--file", type=int)
    select.add_argument("--all", action="store_true")
    command.add_argument("--force", action="store_true")
    command.set_defaults(function=command_extract)

    command = sub.add_parser("import-text", help="append fixed CP037 records to a workspace")
    command.add_argument("work_dir")
    command.add_argument("text_file")
    command.add_argument("--record-length", required=True, type=int,
                         choices=range(1, MAX_BLOCK + 1), metavar="N")
    command.add_argument("--records-per-block", type=int, default=1)
    command.add_argument("--encoding", default="utf-8")
    command.set_defaults(function=command_import_text)

    command = sub.add_parser("add-s36-library",
                             help="add one native SSP LIBRFILE standard-labeled data set")
    command.add_argument("work_dir")
    command.add_argument("data_file")
    command.add_argument("--data-set-id", required=True)
    command.add_argument("--block-length", type=int, default=4096,
                         choices=range(1, MAX_BLOCK + 1), metavar="N")
    command.add_argument("--record-length", type=int, default=256,
                         choices=range(1, MAX_BLOCK + 1), metavar="N")
    command.add_argument("--creation-date", required=True, metavar="YYDDD")
    command.add_argument("--expiration-date", required=True, metavar="YYDDD")
    command.add_argument("--producer", default="FROMLIBR/$MAINT")
    command.add_argument("--system-code", default="IBM SYSTEM/36")
    command.set_defaults(function=command_add_s36_library)

    command = sub.add_parser("labels", help="print labels decoded from EBCDIC bytes")
    command.add_argument("tape_dir")
    command.set_defaults(function=command_labels)
    return top


def main(argv=None):
    try:
        args = parser().parse_args(argv)
        if getattr(args, "records_per_block", 1) < 1:
            fail("records-per-block must be positive")
        args.function(args)
        return 0
    except TapeError as exc:
        print("tape-folder: error: %s" % exc, file=sys.stderr)
        return 2
    except OSError as exc:
        print("tape-folder: error: %s" % exc, file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
