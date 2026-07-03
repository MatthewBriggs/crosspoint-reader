#!/usr/bin/env python3
"""Build a CrossPoint dictionary (.cpdict, format CPD1) from TSV or StarDict input.

The on-device reader (lib/Dictionary/CpDictFile) does a streaming binary search
over the index table, comparing the query against keys with memcmp on raw UTF-8
bytes. The index here MUST therefore be sorted by key.encode("utf-8"), not by
Python string order. Keys are NFC-normalized and casefolded to match the
device-side dictNormalizeKey() output.

File layout (all integers little-endian):
  Header (32 bytes):
    char[4]  magic        "CPD1"
    uint32   entryCount
    uint32   indexOffset  absolute offset of the index table
    uint32   keysOffset   absolute offset of the keys blob
    uint32   defsOffset   absolute offset of the definitions blob
    byte[12] reserved     zero
  Index table: entryCount records of 12 bytes, sorted by key bytes:
    uint32   keyOffset    relative to keysOffset
    uint32   defOffset    relative to defsOffset
    uint16   keyLen       bytes, no NUL, <= MAX_KEY_LEN
    uint16   defLen       bytes, <= MAX_DEF_LEN
  Keys blob: concatenated raw UTF-8 keys
  Defs blob: concatenated raw UTF-8 definitions (senses joined with '\n')

Usage:
  python3 scripts/build_dictionary.py --tsv words.tsv --out en.cpdict
  python3 scripts/build_dictionary.py --stardict path/to/base --out en.cpdict
  (--stardict expects base.ifo / base.idx / base.dict or base.dict.dz)
"""

import argparse
import gzip
import os
import re
import struct
import sys
import unicodedata

MAGIC = b"CPD1"
MAX_KEY_LEN = 63
MAX_DEF_LEN = 4096
HEADER_SIZE = 32
INDEX_RECORD_SIZE = 12


def normalize_key(raw: str) -> str:
    """Mirror of the device-side dictNormalizeKey(): NFC + casefold."""
    return unicodedata.normalize("NFC", raw.strip().casefold())


def truncate_utf8(data: bytes, limit: int) -> bytes:
    """Truncate to <= limit bytes on a UTF-8 codepoint boundary."""
    if len(data) <= limit:
        return data
    cut = limit
    while cut > 0 and (data[cut] & 0xC0) == 0x80:
        cut -= 1
    return data[:cut]


_TAG_RE = re.compile(r"<[^>]{1,64}>")


def strip_markup(text: str) -> str:
    """Minimal Pango/HTML tag removal for StarDict definition payloads."""
    return _TAG_RE.sub("", text)


# GCIDE / dictd "Webster's 1913" notation. These respelling and cross-reference
# markers are meant for a dictionary UI, not plain reading, so we flatten them:
#   \He\           headword respelling delimiters   -> dropped
#   (h[=e])        phonetic codes: [=e] [i^] ['o]... -> base letters (he)
#   {Him}          cross-reference to a defined word -> Him
#   [Obs.] [R.]    usage labels                      -> kept (real annotations)
_INNER_BRACKET_RE = re.compile(r"\[[^\[\]]*\]")
_BACKSLASH_RE = re.compile(r"\\[^\\]{0,40}\\")
_DIACRITIC_RE = re.compile(r"[=^'`~:]")
_SPACE_PUNCT_RE = re.compile(r"\s+([,;.:)])")


def _flatten_bracket(match: "re.Match[str]") -> str:
    inner = match.group(0)[1:-1]
    # Phonetic respelling codes are short (a letter or digraph plus a diacritic
    # marker, e.g. [=e] [i^] ['o] [th]). Longer brackets are real annotations
    # (usage labels like [Obs.], grammar/etymology notes like [AS. ...]) and are
    # kept verbatim — only their nested phonetic codes and braces get flattened.
    if len(inner) <= 4 and (_DIACRITIC_RE.search(inner) or re.fullmatch(r"[a-zA-Z]{1,3}", inner)):
        return re.sub(r"[^a-zA-Z]", "", inner)
    return match.group(0)


def clean_gcide(text: str) -> str:
    """Flatten Webster's 1913 (dictd/GCIDE) respelling and reference markup."""
    text = _BACKSLASH_RE.sub("", text)
    # Resolve innermost brackets repeatedly so nested cases like
    # "[nom. {His} (h[i^]z)]" collapse from the inside out.
    prev = None
    while prev != text:
        prev = text
        text = _INNER_BRACKET_RE.sub(_flatten_bracket, text)
    text = text.replace("{", "").replace("}", "")
    # The source hard-wraps lines mid-sentence; fold those single newlines into
    # spaces so the reader can re-wrap, but keep blank-line paragraph breaks
    # (sense boundaries) as single newlines for the overlay to render.
    paragraphs = []
    for para in re.split(r"\n[ \t]*\n", text):
        para = _SPACE_PUNCT_RE.sub(r"\1", re.sub(r"\s+", " ", para)).strip()
        if para:
            paragraphs.append(para)
    return "\n".join(paragraphs)


def read_tsv(path: str):
    """Yield (word, definition) pairs from a word<TAB>definition file."""
    with open(path, "r", encoding="utf-8") as fh:
        for lineno, line in enumerate(fh, 1):
            line = line.rstrip("\n")
            if not line or line.startswith("#"):
                continue
            if "\t" not in line:
                print(f"warning: line {lineno} has no tab, skipped", file=sys.stderr)
                continue
            word, definition = line.split("\t", 1)
            yield word, definition


def read_stardict(base: str, clean: bool = True):
    """Yield (word, definition) pairs from base.ifo/.idx/.dict[.dz]."""
    ifo_path = base + ".ifo"
    idx_path = base + ".idx"
    with open(ifo_path, "r", encoding="utf-8") as fh:
        ifo = dict(
            line.strip().split("=", 1) for line in fh if "=" in line
        )
    if ifo.get("idxoffsetbits", "32") != "32":
        raise SystemExit("64-bit StarDict idx files are not supported")

    if os.path.exists(base + ".dict.dz"):
        # dictzip is gzip-compatible when read start-to-end
        with gzip.open(base + ".dict.dz", "rb") as fh:
            dict_data = fh.read()
    else:
        with open(base + ".dict", "rb") as fh:
            dict_data = fh.read()

    with open(idx_path, "rb") as fh:
        idx = fh.read()
    pos = 0
    while pos < len(idx):
        end = idx.index(b"\0", pos)
        word = idx[pos:end].decode("utf-8", errors="replace")
        offset, size = struct.unpack(">II", idx[end + 1 : end + 9])
        pos = end + 9
        definition = dict_data[offset : offset + size].decode(
            "utf-8", errors="replace"
        )
        definition = strip_markup(definition)
        if clean:
            definition = clean_gcide(definition)
        yield word, definition


def build(entries, out_path: str, limit: int = 0) -> None:
    merged: dict[bytes, bytes] = {}
    dropped_long = 0
    for word, definition in entries:
        key = normalize_key(word).encode("utf-8")
        if not key:
            continue
        if len(key) > MAX_KEY_LEN:
            dropped_long += 1
            continue
        definition = definition.strip()
        if not definition:
            continue
        body = definition.encode("utf-8")
        if key in merged:
            merged[key] = merged[key] + b"\n" + body
        else:
            merged[key] = body

    keys = sorted(merged)  # bytes sort == memcmp order on device
    if limit:
        keys = keys[:limit]

    index = bytearray()
    keys_blob = bytearray()
    defs_blob = bytearray()
    for key in keys:
        body = truncate_utf8(merged[key], MAX_DEF_LEN)
        index += struct.pack(
            "<IIHH", len(keys_blob), len(defs_blob), len(key), len(body)
        )
        keys_blob += key
        defs_blob += body

    index_offset = HEADER_SIZE
    keys_offset = index_offset + len(index)
    defs_offset = keys_offset + len(keys_blob)
    header = struct.pack(
        "<4sIIII12x", MAGIC, len(keys), index_offset, keys_offset, defs_offset
    )
    assert len(header) == HEADER_SIZE

    with open(out_path, "wb") as fh:
        fh.write(header)
        fh.write(index)
        fh.write(keys_blob)
        fh.write(defs_blob)

    size_kb = (HEADER_SIZE + len(index) + len(keys_blob) + len(defs_blob)) / 1024
    print(f"{out_path}: {len(keys)} entries, {size_kb:.0f} KB")
    if dropped_long:
        print(f"dropped {dropped_long} keys longer than {MAX_KEY_LEN} bytes")


def verify(path: str) -> None:
    """Read-back sanity check: header fields consistent, keys sorted."""
    with open(path, "rb") as fh:
        data = fh.read()
    magic, count, index_off, keys_off, defs_off = struct.unpack_from(
        "<4sIIII", data, 0
    )
    assert magic == MAGIC, "bad magic"
    assert index_off == HEADER_SIZE
    assert keys_off == index_off + count * INDEX_RECORD_SIZE
    prev = b""
    for i in range(count):
        key_off, def_off, key_len, def_len = struct.unpack_from(
            "<IIHH", data, index_off + i * INDEX_RECORD_SIZE
        )
        key = data[keys_off + key_off : keys_off + key_off + key_len]
        assert prev <= key, f"index not sorted at entry {i}"
        assert defs_off + def_off + def_len <= len(data), f"def overrun at {i}"
        prev = key
    print(f"verify OK: {count} entries sorted, offsets consistent")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--tsv", help="word<TAB>definition input file")
    source.add_argument("--stardict", help="StarDict base path (without .ifo)")
    parser.add_argument("--out", required=True, help="output .cpdict path")
    parser.add_argument("--limit", type=int, default=0, help="cap entry count")
    parser.add_argument(
        "--verify", action="store_true", help="re-read and check the output"
    )
    parser.add_argument(
        "--no-gcide-cleanup",
        action="store_true",
        help="keep raw Webster's/dictd respelling markup in StarDict input",
    )
    args = parser.parse_args()

    entries = (
        read_tsv(args.tsv)
        if args.tsv
        else read_stardict(args.stardict, clean=not args.no_gcide_cleanup)
    )
    build(entries, args.out, args.limit)
    if args.verify:
        verify(args.out)


if __name__ == "__main__":
    main()
