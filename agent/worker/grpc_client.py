
import asyncio
import logging
import os
import socket
import uuid

import grpc

import filewatcher_pb2 as pb
import filewatcher_pb2_grpc as pb_grpc

from unix_client import list_directory


SERVER_ADDRESS = os.getenv(
    "FILEWATCH_SERVER",
    "192.168.124.1:50052"  # im using 50052 with port fowarding in the host of the grpc sever 50052->50051
)

AGENT_ID = os.getenv(
    "FILEWATCH_AGENT_ID",
    socket.gethostname()
)

MAX_PAGE_SIZE = 10000

logger = logging.getLogger("FileWatchWorker")


async def handle_directory_request(request):
    """
    Receive a gRPC DirectoryRequest,
    query the C collector through the UNIX socket,
    and convert the JSON response to Protobuf.
    """

    logger.info(
        "LIST_DIRECTORY path=%s limit=%d cursor=%s",
        request.path,
        request.limit,
        request.cursor or "None"
    )

    limit = request.limit or MAX_PAGE_SIZE

    response = pb.DirectoryResponse(
        request_id=request.request_id,
        path=request.path
    )

    if not 1 <= limit <= MAX_PAGE_SIZE:
        response.error = "Invalid page limit"
        return response

    try:

        result = await asyncio.to_thread(
            list_directory,
            path=request.path,
            limit=limit,
            cursor=request.cursor or None
        )

        if result.get("status") != "ok":
            error = result.get("error", {})

            code = error.get("code", "UNKNOWN")
            message = error.get(
                "message",
                "Unknown collector error"
            )

            response.error = f"{code}: {message}"

            logger.error(
                "Collector error: %s",
                response.error
            )

            return response

        data = result["data"]

        response.path = data["path"]
        response.has_more = data.get("has_more", False)
        response.next_cursor = (
            data.get("next_cursor") or ""
        )
        response.partial = data.get("partial", False)
        response.error_code = data.get("error_code", 0)

        for entry in data["entries"]:
            response.entries.add(
                name=entry["name"],
                type=entry["type"]
            )

        logger.info(
            "Directory response: %d entries, has_more=%s",
            len(response.entries),
            response.has_more
        )

        return response

    except (
        OSError,
        ConnectionError,
        TimeoutError,
        ValueError,
        KeyError,
        TypeError,
        RuntimeError
    ) as exc:

        logger.error(
            "Directory request failed: %s",
            exc
        )

        response.error = str(exc)
        return response


async def run_grpc_client():
    """
    Connect to the central FileWatch server
    and handle commands over a bidirectional stream.
    """

    logger.info("Starting FileWatch Worker")
    logger.info("Agent ID: %s", AGENT_ID)
    logger.info("Server: %s", SERVER_ADDRESS)

    outgoing = asyncio.Queue()

    async def outgoing_messages():
        while True:
            message = await outgoing.get()

            if message is None:
                break

            yield message

    async with grpc.aio.insecure_channel(
        SERVER_ADDRESS,
        options=[
            ("grpc.max_receive_message_length", 4 * 1024 * 1024),
            ("grpc.max_send_message_length", 4 * 1024 * 1024)
        ]
    ) as channel:

        logger.info("Connecting to gRPC server...")

        await asyncio.wait_for(
            channel.channel_ready(),
            timeout=10
        )

        logger.info("Connected to gRPC server")

        stub = pb_grpc.FileWatchServiceStub(channel)

        stream = stub.AgentStream(
            outgoing_messages()
        )

        hello = pb.AgentMessage(
            agent_id=AGENT_ID,
            hello=pb.AgentHello(
                hostname=socket.gethostname()
            )
        )

        await outgoing.put(hello)

        logger.info("Agent registration sent")

        try:
            async for message in stream:

                if message.HasField("directory_request"):

                    request = message.directory_request

                    logger.info(
                        "Command received: LIST_DIRECTORY %s",
                        request.path
                    )

                    result = await handle_directory_request(
                        request
                    )

                    agent_message = pb.AgentMessage(
                        agent_id=AGENT_ID,
                        directory_response=result
                    )

                    await outgoing.put(agent_message)

                    logger.info(
                        "Response sent for request %s",
                        request.request_id
                    )

                else:
                    logger.warning(
                        "Unknown server message"
                    )

        finally:
            await outgoing.put(None)

            logger.info(
                "gRPC stream closed"
            )
