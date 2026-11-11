
import argparse
import time

from unix_client import iter_directory_pages


def main():
    parser = argparse.ArgumentParser(
        description="FileWatch directory explorer"
    )

    parser.add_argument(
        "path",
        nargs="?",
        default="/",
        help="Absolute directory path"
    )

    parser.add_argument(
        "--limit",
        type=int,
        default=10000,
        help="Entries per page (max 10000)"
    )

    parser.add_argument(
        "--count-only",
        action="store_true",
        help="Only display page and entry counts"
    )

    args = parser.parse_args()

    total_entries = 0
    total_pages = 0
    partial_pages = 0

    start = time.perf_counter()

    try:
        for page in iter_directory_pages(
            args.path, limit=args.limit
        ):
            total_pages += 1

            entries = page["entries"]
            total_entries += len(entries)

            if page.get("partial"):
                partial_pages += 1

            print(
                f"[Page {total_pages}] "
                f"{len(entries)} entries"
            )

            if not args.count_only:
                for entry in entries:
                    print(
                        f"  {entry['type']:12} "
                        f"{entry['name']}"
                    )

    except (OSError, ValueError, RuntimeError) as exc:
        print(f"[ERROR] {exc}")
        return 1

    elapsed = time.perf_counter() - start

    print("\n--- FileWatch results ---")
    print(f"Directory:     {args.path}")
    print(f"Total entries: {total_entries}")
    print(f"Total pages:   {total_pages}")
    print(f"Partial pages: {partial_pages}")
    print(f"Elapsed:       {elapsed:.4f} seconds")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
