#include "dbconnector.h"
#include "table.h"

#include "dhcp4relay.h"
#include "dhcp4relay_stats.h"

/* DHCPv4 counter name map */
std::map<int, std::string> counterMap = {
    {DHCPv4_MESSAGE_TYPE_UNKNOWN, "Unknown"},
    {DHCPv4_MESSAGE_TYPE_DISCOVER, "Discover"},
    {DHCPv4_MESSAGE_TYPE_OFFER, "Offer"},
    {DHCPv4_MESSAGE_TYPE_REQUEST, "Request"},
    {DHCPv4_MESSAGE_TYPE_DECLINE, "Decline"},
    {DHCPv4_MESSAGE_TYPE_ACK, "Acknowledge"},
    {DHCPv4_MESSAGE_TYPE_NAK, "NegativeAcknowledge"},
    {DHCPv4_MESSAGE_TYPE_RELEASE, "Release"},
    {DHCPv4_MESSAGE_TYPE_INFORM, "Inform"},
    {DHCPv4_MESSAGE_TYPE_MALFORMED, "Malformed"},
    {DHCPv4_MESSAGE_TYPE_DROP, "Dropped"}
};

/**
 * @code                calculate_delta(uint64_t new_value, uint64_t old_value);
 *
 * @brief               Helper function to calculate safe delta with overflow handling.
 *
.* @param new_value     new uint64_t value
.* @param old_value     old uint64_t value
 *
 * @return              delta value
 */
uint64_t calculate_delta(uint64_t new_value, uint64_t old_value) {
    if (new_value >= old_value) {
        return new_value - old_value;
    } else {
        // Handle overflow case
        return (std::numeric_limits<uint64_t>::max() - old_value) + new_value + 1;
    }
}

/**
 * @code                DHCPCounterTable::dbUpdateLoop();
 *
 * @brief               Loop to update dhcp stats to the DB periodically.
 *                      This loop is triggered by a new thread which is responsible to update stats to DB.
 *
 *                      Mutex lock is taken to copy the data locally, before parsing and setting to DB.
 *
 * @return              none
 */
void DHCPCounterTable::dbUpdateLoop() {
    std::shared_ptr<swss::DBConnector> state_db = std::make_shared<swss::DBConnector> ("STATE_DB", 0);
    std::shared_ptr<swss::Table> cntrTable = std::make_shared<swss::Table> (
        state_db.get(), "DHCPV4_COUNTER_TABLE"
    );

    while (!stopThread) {
        std::this_thread::sleep_for(std::chrono::seconds(DHCP_RELAY_DB_UPDATE_TIMER_VAL));

        // Copy the data by taking lock for processing and populating to DB
        std::unordered_map<std::string, DHCPCounters> interfacesCopy;
        {
            std::lock_guard<std::mutex> lock(interfacesMutex);
            interfacesCopy = interfacesCntrTable;
        }

        /* These steps are followed before updating to Redis:
           1. Fetch present values from redis - existing _fields
           2. Update counters with 'cache values' + 'existing_feilds'
           3. Populate to DB
        */
        for (const auto& [interface, counters] : interfacesCopy) {
            // RX counters
            std::vector<swss::FieldValueTuple> existing_rx_fields;
            std::string rx_key = interface + "|RX";
            cntrTable->get(rx_key, existing_rx_fields);

            std::vector<swss::FieldValueTuple> rx_fields;
            std::unordered_map<std::string, int> rx_counter_map;

            // Populate existing counters
            for (const auto& field : existing_rx_fields) {
                rx_counter_map[fvField(field)] = std::stoi(fvValue(field));
            }

            // Update with new values
            for (const auto& [type, value] : counters.RX) {
                rx_counter_map[type] += value;
                rx_fields.emplace_back(type, std::to_string(rx_counter_map[type]));
            }

            cntrTable->set(rx_key, rx_fields);

            // TX counters (similar logic)
            std::vector<swss::FieldValueTuple> existing_tx_fields;
            std::string tx_key = interface + "|TX";
            cntrTable->get(tx_key, existing_tx_fields);

            std::vector<swss::FieldValueTuple> tx_fields;
            std::unordered_map<std::string, int> tx_counter_map;

            // Populate existing counters
            for (const auto& field : existing_tx_fields) {
                tx_counter_map[fvField(field)] = std::stoi(fvValue(field));
            }

            // Update with new values
            for (const auto& [type, value] : counters.TX) {
                tx_counter_map[type] += value;
                tx_fields.emplace_back(type, std::to_string(tx_counter_map[type]));
            }

            cntrTable->set(tx_key, tx_fields);
        }

        // Update local changes after syncing to Redis
        // We will take the delta values of running data in interfacesCntrTable
        // and previosuly copied interfacesCopy values.
        {
            // Taking lock inside a block so that is released automatically
            std::lock_guard<std::mutex> lock(interfacesMutex);
            for (auto& [interface, counters] : interfacesCntrTable) {
                for (auto& [type, value] : counters.RX) {
                    value = calculate_delta(value, interfacesCopy[interface].RX[type]);
                }
                for (auto& [type, value] : counters.TX) {
                    value = calculate_delta(value, interfacesCopy[interface].TX[type]);
                }
            }
        }
        syslog(LOG_INFO, "DHCPV4_RELAY: DHCPCounterTable::dbUpdateLoop() : Data Updated to DB \n");
    }
}

/**
 * @code                DHCPCounterTable::startDbUpdates();
 *
 * @brief               Method to start thread to update stats to DB periodically.
 *
 * @return              none
 */
void DHCPCounterTable::startDbUpdates() {
    dbUpdateThread = std::thread(&DHCPCounterTable::dbUpdateLoop, this);
}

/**
 * @code                DHCPCounterTable::stopDbUpdates();
 *
 * @brief               Method to stop thread which updates stats to DB periodically.
 *
 * @return              none
 */
void DHCPCounterTable::stopDbUpdates() {
    stopThread = true;
    if (dbUpdateThread.joinable()) {
        dbUpdateThread.join();
    }
}

/**
 * @code                DHCPCounterTable::initializeInterface(std::string& interface);
 *
 * @brief               Method to initialize counters in DB for a particular interface
 *
.* @param interface     Name of the interface for which RX and TX counters need to be initialized
 *
 * @return              none
 */
void DHCPCounterTable::initializeInterface(const std::string& interface) {
    std::lock_guard<std::mutex> lock(interfacesMutex);
    DHCPCounters counter;
    for (const auto& type : counterMap) {
        counter.RX[type.second] = 0;
        counter.TX[type.second] = 0;
    }
    interfacesCntrTable[interface] = counter;
}

/**
 * @code                DHCPCounterTable::incrementCounter(const std::string& interface,
 *                                                          const std::string& direction,
 *                                                          int msg_type);
 *
 * @brief               Method to increment counters in DB for a particular interface,
 *                      direction(RX|TX) and dhcp_message_type_t
 *
.* @param interface     Name of the interface for which counters need to be incremented
 *
 * @return              none
 */
void DHCPCounterTable::incrementCounter(const std::string& interface,
                                        const std::string& direction,
                                        int msg_type) {
    std::string type = counterMap.find(msg_type)->second;
    // Initialize counters if not present
    if (interfacesCntrTable.find(interface) == interfacesCntrTable.end())
        DHCPCounterTable::initializeInterface(interface);

    std::lock_guard<std::mutex> lock(interfacesMutex);

    if (direction == "RX") {
        interfacesCntrTable[interface].RX[type]++;
    } else if (direction == "TX") {
        interfacesCntrTable[interface].TX[type]++;
    }
}

/**
 * @code                DHCPCounterTable::removeInterface(const std::string& interface);
 *
 * @brief               Method to remove counters from global datastructure.
 *
.* @param interface     Name of the interface for which counters need to be removed.
 *
 * @return              none
 */
void DHCPCounterTable::removeInterface(const std::string& interface) {
    std::lock_guard<std::mutex> lock(interfacesMutex);
    interfacesCntrTable.erase(interface);
}

/**
 * @code                DHCPCounterTable:~DHCPCounterTable()
 *
 * @brief               Destructor.
 *
 * @return              none
 */
DHCPCounterTable::~DHCPCounterTable() {
    stopDbUpdates();
}
