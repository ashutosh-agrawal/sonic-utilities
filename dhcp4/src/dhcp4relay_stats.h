#pragma once

#include <unordered_map>
#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <atomic>
#include <limits>

#define DHCP_RELAY_DB_UPDATE_TIMER_VAL 30

struct DHCPCounters {
    std::unordered_map<std::string, uint64_t> RX;
    std::unordered_map<std::string, uint64_t> TX;
};

class DHCPCounterTable {
private:
    std::unordered_map<std::string, DHCPCounters> interfacesCntrTable;
    std::mutex interfacesMutex;
    std::atomic<bool> stopThread{false};
    std::thread dbUpdateThread;
    
    void dbUpdateLoop();

public:
    void startDbUpdates();
    void stopDbUpdates();
    void initializeInterface(const std::string& interface);
    void incrementCounter(const std::string& interface, const std::string& direction, 
                          int msg_type);
    void removeInterface(const std::string& interface);

    ~DHCPCounterTable();
};

uint64_t calculate_delta(uint64_t new_value, uint64_t old_value);
