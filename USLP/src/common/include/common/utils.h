#pragma once

#include <stdint.h> 
#include <iostream>
#include <bitset>
#include <vector>
#include <array>
#include <cstdint>
#include <cassert>
#include <random>
#include <cstring>
#include <algorithm>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <queue>
#include <condition_variable>

constexpr int TEST_ARRAY_SIZE = 3009;

int8_t GetChannelByVCID(uint8_t vcid, std::array<int8_t, MAX_VC_COUNT> &mapping); // Returns the channel index of the VCID, -1 if invalid
void printBytes(uint64_t value);
template <size_t M, size_t N> void append(const BitBuffer<M>& src, BitBuffer<N>& dest, size_t& offset);
std::array<uint8_t, 4> CRCGenerator();
// Generates random bytes enough to fill 3 transfer frames with maximum capacity
std::array<uint8_t, TEST_ARRAY_SIZE> GenerateRandomBytes();
// Writes Bytes to .txt assuming maximum transfer frame capacity
void WriteBytes(std::array<uint8_t, TEST_ARRAY_SIZE> message);

// Definition lives here because it is a template
template <size_t M, size_t N> void append(const BitBuffer<M>& src, BitBuffer<N>& dest, size_t& offset) {
	std::memcpy(&dest.data[offset], &src.data[0], src.length);
	offset += src.length;
};

template <typename T>
class ThreadSafeMultiplexerQueue {
private:
    std::queue<T> m_queue;
    std::mutex m_mutex;
    std::condition_variable m_cond;

public:
    void push(T&& value) {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            // Move the object into the underlying container
            m_queue.push(std::move(value));
        }
        m_cond.notify_one(); // Wake up the multiplexing thread
    }

    // Returns true if an item was successfully popped, 
    // false if the timeout expired first.
    bool pop_with_timeout(T& value, std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(m_mutex);
        
        // Wait until there is data OR the timeout expires
        bool dataAvailable = m_cond.wait_for(lock, timeout, [this] { 
            return !m_queue.empty();
        });
        
        if (!dataAvailable) {
            return false;
        }
        
        value = std::move(m_queue.front());
        m_queue.pop();
        return true;
    }
    
    bool empty() {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_queue.empty();
    }
};