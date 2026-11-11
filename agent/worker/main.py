
import sys
import json

from unix_client import list_directory


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else "/"

    try:
        response = list_directory(path)

        if response.get("status") == "ok":
            data = response["data"]

            print(f"\nDirectory: {data['path']}\n")

            for entry in data["entries"]:
                print(f"{entry['type']:12} {entry['name']}")

            if data.get("partial"):
                print(
                    "\nWarning: partial listing, error code:",
                    data.get("error_code")
                )
        else:
            print("Error:", response.get("message"))

    except (OSError, ValueError, ConnectionError,
            json.JSONDecodeError) as exc:
        print("Communication error:", exc)
        sys.exit(1)


if __name__ == "__main__":
    main()
