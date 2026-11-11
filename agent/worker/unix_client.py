
import json
import socket
import struct
import uuid

SOCKET_PATH = "/tmp/filewatch-collector.sock"
MAX_MESSAGE_SIZE = 1024 * 1024
DEFAULT_PAGE_SIZE = 10000
MAX_PAGE_SIZE = 10000


def recv_exact(sock, size):
    data = bytearray()

    while len(data) < size:
        chunk = sock.recv(size - len(data))

        if not chunk:
            raise ConnectionError("Socket closed unexpectedly")

        data.extend(chunk)

    return bytes(data)


def list_directory(path, limit=DEFAULT_PAGE_SIZE, cursor=None):
    """Retrieve one page from the C collector."""

    if not isinstance(path, str) or not path.startswith("/"):
        raise ValueError("An absolute directory path is required")

    if not isinstance(limit, int) or isinstance(limit, bool):
        raise ValueError("Invalid page limit")

    if not 1 <= limit <= MAX_PAGE_SIZE:
        raise ValueError("Page limit out of range")

    request_id = str(uuid.uuid4())

    request = {
        "request_id": request_id,
        "command": "LIST_DIRECTORY",
        "path": path,
        "limit": limit,
        "cursor": cursor
    }

    payload = json.dumps(
        request, ensure_ascii=True
    ).encode("utf-8")

    if len(payload) > MAX_MESSAGE_SIZE:
        raise ValueError("Request exceeds size limit")

    header = struct.pack("!I", len(payload))

    with socket.socket(
        socket.AF_UNIX, socket.SOCK_STREAM
    ) as sock:

        sock.settimeout(5)
        sock.connect(SOCKET_PATH)

        sock.sendall(header + payload)

        response_header = recv_exact(sock, 4)
        response_size = struct.unpack(
            "!I", response_header
        )[0]

        if not 0 < response_size <= MAX_MESSAGE_SIZE:
            raise ValueError("Invalid response size")

        response_data = recv_exact(sock, response_size)

    response = json.loads(response_data.decode("utf-8"))

    if response.get("request_id") != request_id:
        raise ValueError("Mismatched request ID")

    return response


def iter_directory_pages(path, limit=DEFAULT_PAGE_SIZE):
    """Yield directory pages until pagination finishes."""

    cursor = None

    while True:
        response = list_directory(
            path,
            limit=limit,
            cursor=cursor
        )

        if response.get("status") != "ok":
            error = response.get("error", {})

            raise RuntimeError(
                f"{error.get('code', 'UNKNOWN')}: "
                f"{error.get('message', 'Listing failed')}"
            )

        data = response["data"]

        yield data

        if not data.get("has_more", False):
            break

        cursor = data.get("next_cursor")

        if not isinstance(cursor, str) or not cursor:
            raise RuntimeError("Missing pagination cursor")
