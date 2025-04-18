#include <sstream>
#include <syslog.h>
#include <algorithm>
#include "dhcp4_config_interface.h"

constexpr auto DEFAULT_TIMEOUT_MSEC = 1000;

bool pollSwssNotifcation = true;
swss::Select swssSelect;

/**
 * @code                void initialize_swss()
 *
 * @brief               initialize DB tables and start SWSS listening thread
 *
 * @return              none
 */
void initialize_swss(std::unordered_map<std::string, relay_config> &vlans)
{
    try {
        std::shared_ptr<swss::DBConnector> configDbPtr = std::make_shared<swss::DBConnector> ("CONFIG_DB", 0);
        swss::SubscriberStateTable ipHelpersTable(configDbPtr.get(), "DHCPV4_RELAY");
        swssSelect.addSelectable(&ipHelpersTable);
        get_dhcp(vlans, &ipHelpersTable, false);
    }
    catch (const std::bad_alloc &e) {
        syslog(LOG_ERR, "[DHCPV4_RELAY] Failed allocate memory. Exception details: %s", e.what());
    }
}

/**
*@code      stopSwssNotificationPoll
*
*@brief     stop SWSS listening thread
*
*@return    none
*/
static void stopSwssNotificationPoll() {
    pollSwssNotifcation = false;
};

/**
 * @code                void deinitialize_swss()
 *
 * @brief               deinitialize DB interface and join SWSS listening thread
 *
 * @return              none
 */
void deinitialize_swss()
{
    stopSwssNotificationPoll();

}

/**

 * @code                void get_dhcp(std::unordered_map<std::string, relay_config> &vlans, swss::SubscriberStateTable *ipHelpersTable, bool dynamic)
 *
 * @brief               initialize and get vlan table information from DHCPV4_RELAY
 *
 * @return              none
 */
void get_dhcp(std::unordered_map<std::string, relay_config> &vlans, swss::SubscriberStateTable *ipHelpersTable, bool dynamic) {
    swss::Selectable *selectable;
    int ret = swssSelect.select(&selectable, DEFAULT_TIMEOUT_MSEC);
    if (ret == swss::Select::ERROR) {
        syslog(LOG_WARNING, "[DHCPV4_RELAY] Select: returned ERROR");
        return;
    } else if (ret == swss::Select::TIMEOUT) {
    }
    if (selectable == static_cast<swss::Selectable *> (ipHelpersTable)) {
        if (!dynamic) {
            handleRelayNotification(*ipHelpersTable, vlans);
        } else {
            syslog(LOG_WARNING, "[DHCPV4_RELAY] relay config changed, "
                   "need restart container to take effect");
        }
    }
}

/**
 * @code                    void handleRelayNotification(swss::SubscriberStateTable &ipHelpersTable, std::unordered_map<std::string, relay_config> &vlans)
 *
 * @brief                   handles DHCPv4 relay configuration change notification
 *
 * @param ipHelpersTable    DHCP table
 * @param vlans             map of vlans/argument config that contains strings of server and option
 *
 * @return                  none
 */
void handleRelayNotification(swss::SubscriberStateTable &ipHelpersTable, std::unordered_map<std::string, relay_config> &vlans)
{
    std::deque<swss::KeyOpFieldsValuesTuple> entries;

    ipHelpersTable.pops(entries);
    processRelayNotification(entries, vlans);
}

/**
 * @code                    void processRelayNotification(std::deque<swss::KeyOpFieldsValuesTuple> &entries, std::unordered_map<std::string, relay_config> vlans)
 *
 * @brief                   process DHCPv4 relay servers and options configuration change notification
 *
 * @param entries           queue of std::tuple<std::string, std::string, std::vector<FieldValueTuple>> entries in DHCP table
 * @param vlans             map of vlans/argument config that contains strings of server and option
 *
 * @return                  none
 */
void processRelayNotification(std::deque<swss::KeyOpFieldsValuesTuple> &entries, std::unordered_map<std::string, relay_config> &vlans)
{
    std::vector<std::string> servers;

    for (auto &entry: entries) {
        std::string vlan = kfvKey(entry);
        std::string operation = kfvOp(entry);
        std::vector<swss::FieldValueTuple> fieldValues = kfvFieldsValues(entry);

        relay_config intf;
        intf.vlan = vlan;
        intf.state_db = nullptr;
        for (auto &fieldValue: fieldValues) {
            std::string f = fvField(fieldValue);
            std::string v = fvValue(fieldValue);
            if(f == "dhcpv4_servers") {
                std::stringstream ss(v);
                while (ss.good()) {
                    std::string substr;
                    getline(ss, substr, ',');
                    intf.servers.push_back(substr);
                }
                syslog(LOG_DEBUG, "[DHCPV4_RELAY] key: %s, Operation: %s, f: %s, v: %s", vlan.c_str(), operation.c_str(), f.c_str(), v.c_str());
            }
            if(f == "server_vrf") {
                intf.vrf = v;
                syslog(LOG_DEBUG, "[DHCPV4_RELAY] key: %s, Operation: %s, f: %s, v: %s", vlan.c_str(), operation.c_str(), f.c_str(), v.c_str());
            }
            if(f == "source_interface") {
                intf.source_interface = v;
                syslog(LOG_DEBUG, "[DHCPV4_RELAY] key: %s, Operation: %s, f: %s, v: %s", vlan.c_str(), operation.c_str(), f.c_str(), v.c_str());
            }
            if(f == "link_selection") {
                intf.link_selection_opt = v;
                syslog(LOG_DEBUG, "[DHCPV4_RELAY] key: %s, Operation: %s, f: %s, v: %s", vlan.c_str(), operation.c_str(), f.c_str(), v.c_str());
            }
            if(f == "server_id_override") {
                intf.server_id_override_opt = v;
                syslog(LOG_DEBUG, "[DHCPV4_RELAY] key: %s, Operation: %s, f: %s, v: %s", vlan.c_str(), operation.c_str(), f.c_str(), v.c_str());
            }
            if(f == "vrf_selection") {
                intf.vrf_selection_opt = v;
                syslog(LOG_DEBUG, "[DHCPV4_RELAY] key: %s, Operation: %s, f: %s, v: %s", vlan.c_str(), operation.c_str(), f.c_str(), v.c_str());
            }
            if(f == "agent_relay_mode") {
                intf.agent_relay_mode = v;
                syslog(LOG_DEBUG, "[DHCPV4_RELAY] key: %s, Operation: %s, f: %s, v: %s", vlan.c_str(), operation.c_str(), f.c_str(), v.c_str());
            }
        }
        if (intf.servers.empty()) {
            syslog(LOG_WARNING, "[DHCPV4_RELAY] No servers found for VLAN %s, skipping configuration.", vlan.c_str());
            continue;
        }
        syslog(LOG_INFO, "[DHCPV4_RELAY] add %s relay config\n", vlan.c_str());
        vlans[vlan] = intf;
    }
}
