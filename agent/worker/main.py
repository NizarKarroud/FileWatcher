
import asyncio
import logging

import grpc

from grpc_client import run_grpc_client


logging.basicConfig(
    level=logging.INFO,
    format="[%(asctime)s] %(levelname)s: %(message)s"
)


async def main():
    try:
        await run_grpc_client()

    except grpc.aio.AioRpcError as exc:
        logging.error(
            "gRPC error: %s - %s",
            exc.code(),
            exc.details()
        )

    except (OSError, asyncio.TimeoutError) as exc:
        logging.error(
            "Connection error: %s",
            exc
        )

    except asyncio.CancelledError:
        logging.info("Worker cancelled")
        raise


if __name__ == "__main__":
    try:
        asyncio.run(main())

    except KeyboardInterrupt:
        print("\n[FileWatch] Worker stopped")
