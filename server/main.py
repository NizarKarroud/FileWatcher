
import asyncio
import logging
import os
import uuid

import grpc

import filewatcher_pb2 as pb
import filewatcher_pb2_grpc as pb_grpc

# Logging configuration
logging.basicConfig(
    level=logging.INFO,
    format="[%(asctime)s] %(levelname)s: %(message)s"
)

logger = logging.getLogger("FileWatchServer")

GRPC_HOST = os.getenv("GRPC_HOST", "0.0.0.0")
GRPC_PORT = int(os.getenv("GRPC_PORT", "50051"))


class FileWatchService(pb_grpc.FileWatchServiceServicer):

    async def AgentStream(self, request_iterator, context):

        agent_id = None
        registered = False
        request_id = None

        logger.info("New agent stream connected")

        try:
            async for message in request_iterator:

                # Agent registration
                if message.HasField("hello"):

                    if registered:
                        await context.abort(
                            grpc.StatusCode.INVALID_ARGUMENT,
                            "Agent already registered"
                        )

                    if not message.agent_id:
                        await context.abort(
                            grpc.StatusCode.INVALID_ARGUMENT,
                            "Missing agent ID"
                        )

                    agent_id = message.agent_id
                    registered = True

                    logger.info(
                        "Agent registered: %s | Hostname: %s",
                        agent_id,
                        message.hello.hostname
                    )

                    # Send initial test command
                    request_id = str(uuid.uuid4())

                    command = pb.ServerMessage(
                        directory_request=pb.DirectoryRequest(
                            request_id=request_id,
                            path="/etc",
                            limit=10000,
                            cursor=""
                        )
                    )

                    logger.info(
                        "Sending LIST_DIRECTORY /etc to agent %s",
                        agent_id
                    )

                    yield command

                # Directory response from agent
                elif message.HasField("directory_response"):

                    if not registered:
                        await context.abort(
                            grpc.StatusCode.FAILED_PRECONDITION,
                            "Agent not registered"
                        )

                    response = message.directory_response

                    if response.request_id != request_id:
                        logger.warning(
                            "Unexpected response ID from %s: %s",
                            agent_id,
                            response.request_id
                        )
                        continue

                    logger.info(
                        "Directory response from agent %s",
                        agent_id
                    )

                    if response.error:
                        logger.error(
                            "Listing failed: %s",
                            response.error
                        )
                        return

                    logger.info(
                        "Path: %s | Entries: %d | Has more: %s",
                        response.path,
                        len(response.entries),
                        response.has_more
                    )

                    for entry in response.entries[:20]:
                        logger.info(
                            "  %-12s %s",
                            entry.type,
                            entry.name
                        )

                    if len(response.entries) > 20:
                        logger.info(
                            "  ... and %d additional entries",
                            len(response.entries) - 20
                        )

                    if response.partial:
                        logger.warning(
                            "Partial listing, error code: %d",
                            response.error_code
                        )

                    # Continue requesting pages when necessary
                    if response.has_more:

                        if not response.next_cursor:
                            logger.error(
                                "Missing pagination cursor"
                            )
                            return

                        request_id = str(uuid.uuid4())

                        next_command = pb.ServerMessage(
                            directory_request=pb.DirectoryRequest(
                                request_id=request_id,
                                path=response.path,
                                limit=10000,
                                cursor=response.next_cursor
                            )
                        )

                        logger.info(
                            "Requesting next page from %s",
                            agent_id
                        )

                        yield next_command

                    else:
                        logger.info(
                            "Directory listing completed for %s",
                            agent_id
                        )

                        # End the stream after this test
                        return

                else:
                    logger.warning(
                        "Unknown message received from agent %s",
                        agent_id or "unregistered"
                    )

        except asyncio.CancelledError:
            logger.info(
                "Stream cancelled for agent %s",
                agent_id or "unknown"
            )
            raise

        except grpc.RpcError:
            raise

        except Exception:
            logger.exception(
                "Unexpected error in agent stream"
            )
            raise

        finally:
            logger.info(
                "Agent disconnected: %s",
                agent_id or "unknown"
            )


async def serve():

    server = grpc.aio.server(
        options=[
            ("grpc.max_receive_message_length", 4 * 1024 * 1024),
            ("grpc.max_send_message_length", 4 * 1024 * 1024)
        ]
    )

    pb_grpc.add_FileWatchServiceServicer_to_server(
        FileWatchService(),
        server
    )

    address = f"{GRPC_HOST}:{GRPC_PORT}"

    bound_port = server.add_insecure_port(address)

    if bound_port == 0:
        raise RuntimeError(
            f"Unable to bind gRPC server to {address}"
        )

    await server.start()

    logger.info("FileWatch gRPC server started")
    logger.info("Listening on %s", address)

    try:
        await server.wait_for_termination()

    finally:
        logger.info("Stopping gRPC server")
        await server.stop(grace=5)


if __name__ == "__main__":
    asyncio.run(serve())
