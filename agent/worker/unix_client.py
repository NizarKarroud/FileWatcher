
import json
import socket
import struct
import uuid

SOCKET_PATH = "/tmp/filewatch-collector.sock"
MAX_MESSAGE_SIZE = 1024 * 1024


def recv_exact(sock, size):
    """Lire exactement size octets."""

    data = bytearray()

    while len(data) < size:
        chunk = sock.recv(size - len(data))

        if not chunk:
            raise ConnectionError("Socket closed unexpectedly")

        data.extend(chunk)

    return bytes(data)


def list_directory(path):
    """Demander le contenu d'un répertoire au collecteur C."""

    request_id = str(uuid.uuid4())

    request = {
        "request_id": request_id,
        "command": "LIST_DIRECTORY",
        "path": path
    }

    payload = json.dumps(request).encode("utf-8")

    if len(payload) > MAX_MESSAGE_SIZE:
        raise ValueError("Request too large")

    header = struct.pack("!I", len(payload))

    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as sock:

        sock.settimeout(5)
        sock.connect(SOCKET_PATH)

        sock.sendall(header + payload)

        response_header = recv_exact(sock, 4)
        response_size = struct.unpack("!I", response_header)[0]

        if not 0 < response_size <= MAX_MESSAGE_SIZE:
            raise ValueError("Invalid response size")

        response_data = recv_exact(sock, response_size)

    response = json.loads(response_data.decode("utf-8"))

    if response.get("request_id") != request_id:
        raise ValueError("Mismatched request ID")

    return response
