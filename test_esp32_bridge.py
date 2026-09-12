
"""
test_esp32_bridge.py
======================
Simple standalone test for the ESP32 WiFi-to-UART bridge, BEFORE any
STM32 involvement. With ESP32 GPIO17(TX) jumpered directly to
GPIO16(RX) - a hardware loopback - anything sent here should echo
straight back through the ESP32's UART bridge.

This proves the WiFi<->UART path works in isolation, the same way we
proved the MCP2515's internal loopback mode before ever wiring a real
two-node CAN bus.
"""

import socket
import sys
import time

def main():
    if len(sys.argv) < 2:
        print("Usage: python3 test_esp32_bridge.py <ESP32_IP> [port]")
        sys.exit(1)

    esp32_ip = sys.argv[1]
    port = int(sys.argv[2]) if len(sys.argv) > 2 else 8888

    print(f"Connecting to ESP32 bridge at {esp32_ip}:{port}...")
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.settimeout(5)
    sock.connect((esp32_ip, port))
    print("Connected.")

    test_message = b"HELLO_ESP32_BRIDGE_TEST\n"
    print(f"Sending: {test_message}")
    sock.sendall(test_message)

    try:
        sock.settimeout(3)
        response = sock.recv(256)
        print(f"Received: {response}")

        if response == test_message:
            print("\nPASS: Loopback echo matches exactly - bridge is working correctly.")
        else:
            print("\nFAIL: Response does not match what was sent.")
    except socket.timeout:
        print("\nFAIL: No response received within timeout.")
        print("Check: is the GPIO17->GPIO16 loopback jumper connected on the ESP32?")

    sock.close()

if __name__ == "__main__":
    main()
