#include <common/uslp.h>

void USLP::InitNetworkSocket() {
    // 1. Create a UDP Datagram Socket
    m_socketFd = socket(AF_INET, SOCK_DGRAM, 0);
    if (m_socketFd < 0) {
        std::cerr << "[ERROR] Failed to create UDP socket\n";
        return;
    }

    // 2. Configure target destination (localhost:5000)
    m_gnuRadioAddr.sin_family = AF_INET;
    m_gnuRadioAddr.sin_port = htons(5000); // Port matching GNU Radio UDP Source
    m_gnuRadioAddr.sin_addr.s_addr = inet_addr("127.0.0.1");
    
    std::cout << "[INFO] UDP Network interface initialized to localhost:5000\n";
}

void USLP::CleanupNetworkSocket() {
    if (m_socketFd >= 0) {
        close(m_socketFd);
        m_socketFd = -1;
    }
}

void USLP::SendToGNURadio(const BitBuffer<MAX_TRANSFER_FRAME_LENGTH>& serializedBytes) {
    std::cout << "Sending bytes to GNU Radio Module...\n";

    if (m_socketFd < 0) {
        return; // Socket not initialized or closed
    }

    // sendto is inherently thread-safe for isolated destination blocks
    ssize_t sentBytes = sendto(
        m_socketFd, 
        serializedBytes.data.data(), // Pointer to underlying contiguous storage array
        serializedBytes.length,      // Size of data to send (e.g., 1024)
        0, 
        reinterpret_cast<const sockaddr*>(&m_gnuRadioAddr), 
        sizeof(m_gnuRadioAddr)
    );

    if (sentBytes < 0) {
        std::cerr << "[WARNING] VCMultiplexer dropped frame transmission over network socket\n";
    } else {
        std::cout << "Sent bytes to GNU Radio Module!\n";
    }
}