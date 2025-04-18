#pragma once

#include <string>
#include <thread>
#include <mutex>
#include <atomic>
#include <limits>

#include "dbconnector.h"
#include "table.h"
#include "subscriberstatetable.h"
#include "select.h"

#include <unordered_map>
#include <boost/thread.hpp>
#include "dhcp4relay.h"


class DHCPMgr {
private:
    std::atomic<bool> stopThread;
public:
    DHCPMgr() : stopThread(false) {}
    ~DHCPMgr();

    void initialize_config_listner();
    void handleSwssNotification();
    void processRelayNotification(std::deque<swss::KeyOpFieldsValuesTuple> &entries);
    void processInterfaceNotification(std::deque<swss::KeyOpFieldsValuesTuple> &entries);
    void processDeviceMetadataNotification(std::deque<swss::KeyOpFieldsValuesTuple> &entries);
};


