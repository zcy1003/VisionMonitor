import argparse
import math
import socket
import time


def build_payload(tick: int) -> str:
    temperature = 82.0 + 8.0 * math.sin(tick * 0.15)
    pressure = 2.05 + 0.18 * math.cos(tick * 0.11)
    speed = 1200.0 + 80.0 * math.sin(tick * 0.09)
    return f"temp={temperature:.2f},pressure={pressure:.3f},speed={speed:.0f}\n"


def run_server(host: str, port: int, interval: float) -> None:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as server:
        server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        server.bind((host, port))
        server.listen(1)
        print(f"TCP sensor server listening on {host}:{port}")

        while True:
            connection, address = server.accept()
            print(f"client connected: {address[0]}:{address[1]}")
            with connection:
                tick = 0
                while True:
                    try:
                        connection.sendall(build_payload(tick).encode("utf-8"))
                    except (BrokenPipeError, ConnectionResetError):
                        print("client disconnected")
                        break
                    tick += 1
                    time.sleep(interval)


def main() -> None:
    parser = argparse.ArgumentParser(description="Local TCP sensor simulator for VisionMonitor.")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=9000)
    parser.add_argument("--interval", type=float, default=1.0)
    args = parser.parse_args()

    run_server(args.host, args.port, args.interval)


if __name__ == "__main__":
    main()
