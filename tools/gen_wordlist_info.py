import argparse
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("wordlist", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()

    data = args.wordlist.read_bytes()
    count = data.count(b"\n")
    if data and not data.endswith(b"\n"):
        count += 1

    args.output.write_text(
        "#pragma once\n\n"
        f"#define WORDLIST_ENTRY_COUNT {count}u\n"
        f"#define WORDLIST_BYTE_COUNT {len(data)}u\n",
        encoding="ascii",
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
