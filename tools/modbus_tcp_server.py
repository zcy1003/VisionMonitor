import argparse
import math
import socket
import struct
import time


def build_registers(tick: int):
    temperature_x10 = int((82.0 + 8.0 * math.sin(tick * 0.15)) * 10)
    pressure_x100 = int((2.05 + 0.18 * math.cos(tick * 0.11)) * 100)
    speed_rpm = int(1200.0 + 80.0 * math.sin(tick * 0.09))
    return [temperature_x10, pressure_x100, speed_rpm]


def handle_request(request: bytes, tick: int):
    if len(request) < 12:
        return None

    transaction_id, protocol_id, _length = struct.unpack(">HHH", request[:6])
    unit_id = request[6]
    function_code = request[7]
    start_address, quantity = struct.unpack(">HH", request[8:12])

    if protocol_id != 0 or function_code != 3 or quantity != 3:
        pdu = bytes([function_code | 0x80, 0x01])
    else:
        registers = build_registers(tick)
        data = b"".join(struct.pack(">H", value & 0xFFFF) for value in registers)
        pdu = bytes([function_code, len(data)]) + data

    header = struct.pack(">HHHB", transaction_id, 0, len(pdu) + 1, unit_id)
    return header + pdu


def run_server(host: str, port: int) -> None:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as server:
        server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        server.bind((host, port))
        server.listen(1)
        print(f"Modbus TCP simulator listening on {host}:{port}")

        tick = 0
        while True:
            connection, address = server.accept()
            print(f"client connected: {address[0]}:{address[1]}")
            with connection:
                while True:
                    request = connection.recv(260)
                    if not request:
                        print("client disconnected")
                        break
                    response = handle_request(request, tick)
                    if response:
                        connection.sendall(response)
                    tick += 1
                    time.sleep(0.02)


def main() -> None:
    parser = argparse.ArgumentParser(description="Local Modbus TCP simulator for VisionMonitor.")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=1502)
    args = parser.parse_args()

    run_server(args.host, args.port)


if __name__ == "__main__":
    main()
