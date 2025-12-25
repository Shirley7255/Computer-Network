#pragma once

#include <iostream>
#include <vector>
#include <string>
#include <cstdint>
#include <chrono>

// For Windows Sockets
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
    // Add Linux/macOS headers if needed
#endif

// --- Protocol Constants ---
const int SERVER_PORT = 8888;
const int MAX_BUFFER_SIZE = 1500; // MTU is typically 1500 bytes
const int HEADER_SIZE = 20;
const int MAX_DATA_SIZE = MAX_BUFFER_SIZE - HEADER_SIZE;
const int FLOW_CONTROL_WINDOW_SIZE = 20; // Fixed flow control window (N)
const int PACKET_TIMEOUT_MS = 500; // Timeout for retransmission in milliseconds

// --- Packet Flags ---
enum Flags {
    SYN = 1 << 0, // 1
    ACK = 1 << 1, // 2
    FIN = 1 << 2, // 4
};

// --- Packet Structure ---
#pragma pack(push, 1)
struct Packet {
    uint32_t seq_num;     // Sequence number
    uint32_t ack_num;     // Acknowledgment number
    uint16_t flags;       // SYN, ACK, FIN flags
    uint16_t window_size; // For flow control (optional here, but good practice)
    uint16_t data_len;    // Length of data
    uint16_t checksum;    // Checksum
    char data[MAX_DATA_SIZE];
};
#pragma pack(pop)


// --- Utility Functions ---

// Calculates the 16-bit checksum for a given data buffer.
uint16_t calculate_checksum(const Packet* packet) {
    uint32_t sum = 0;
    const uint16_t* p = reinterpret_cast<const uint16_t*>(packet);
    int size = HEADER_SIZE + packet->data_len;

    // Temporarily set checksum field to 0 for calculation
    uint16_t original_checksum = packet->checksum;
    const_cast<Packet*>(packet)->checksum = 0;

    // Sum all 16-bit words
    while (size > 1) {
        sum += *p++;
        size -= 2;
    }

    // If there's an odd byte, add it
    if (size > 0) {
        sum += *reinterpret_cast<const uint8_t*>(p);
    }

    // Fold 32-bit sum to 16 bits
    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }

    // Restore original checksum
    const_cast<Packet*>(packet)->checksum = original_checksum;

    return static_cast<uint16_t>(~sum);
}

// Verifies if the packet's checksum is valid.
bool verify_checksum(const Packet* packet) {
    return calculate_checksum(packet) == packet->checksum;
}

// Initializes Winsock on Windows
bool initialize_winsock() {
#ifdef _WIN32
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        std::cerr << "WSAStartup failed." << std::endl;
        return false;
    }
#endif
    return true;
}

// Cleans up Winsock on Windows
void cleanup_winsock() {
#ifdef _WIN32
    WSACleanup();
#endif
}
