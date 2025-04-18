
#include "dhcp4relay_mgr.h"

#include <sstream>
#include <algorithm>
constexpr auto DEFAULT_TIMEOUT_MSEC = 1000;

static std::unordered_map<std::string, relay_config> vlansCopy;

#ifdef UNIT_TEST
using namespace swss;
#endif

void DHCPMgr::initialize_config_listner() {
      stopThread = false;
      std::thread mSwssThread(&DHCPMgr::handleSwssNotification, this);
      mSwssThread.detach();
}

void DHCPMgr::handleSwssNotification() {
    std::shared_ptr<swss::DBConnector> configDbPtr = std::make_shared<swss::DBConnector> ("CONFIG_DB", 0);
    
    swss::SubscriberStateTable configDbRelaymgrTable(configDbPtr.get(), "DHCPV4_RELAY");
    swss::SubscriberStateTable configDbInterfaceTable(configDbPtr.get(), "INTERFACE");
    swss::SubscriberStateTable configDbLoopbackTable(configDbPtr.get(), "LOOPBACK_INTERFACE");
    swss::SubscriberStateTable configDbPortchannelTable(configDbPtr.get(), "PORTCHANNEL_INTERFACE");
    swss::SubscriberStateTable configDbDeviceMetadataTable(configDbPtr.get(), "DEVICE_METADATA");

    std::deque<swss::KeyOpFieldsValuesTuple> entries;
    swss::Select swssSelect;
    swssSelect.addSelectable(&configDbRelaymgrTable);
    swssSelect.addSelectable(&configDbInterfaceTable);
    swssSelect.addSelectable(&configDbLoopbackTable);
    swssSelect.addSelectable(&configDbPortchannelTable);
    swssSelect.addSelectable(&configDbDeviceMetadataTable);

    while (!stopThread) {
	swss::Selectable *selectable;
        int ret = swssSelect.select(&selectable, DEFAULT_TIMEOUT_MSEC);

        if (ret == swss::Select::ERROR) {
            syslog(LOG_ERR,"[DHCPV4_RELAY] Error had been returned in select");
            continue;
        } else if (ret == swss::Select::TIMEOUT) {
            continue;
        } else if (ret != swss::Select::OBJECT) {
            syslog(LOG_ERR,"[DHCPV4_RELAY] Unknown return value from Select: %d", ret);
            continue;
        }

	if (selectable == static_cast<swss::Selectable *> (&configDbRelaymgrTable)) {
            configDbRelaymgrTable.pops(entries);
            processRelayNotification(entries);
        } else if (selectable == static_cast<swss::Selectable *> (&configDbInterfaceTable)) {
            configDbInterfaceTable.pops(entries);
            processInterfaceNotification(entries);
        } else if (selectable == static_cast<swss::Selectable *> (&configDbLoopbackTable)) {
            configDbLoopbackTable.pops(entries);
            processInterfaceNotification(entries);
        } else if (selectable == static_cast<swss::Selectable *> (&configDbPortchannelTable)) {
            configDbPortchannelTable.pops(entries);
            processInterfaceNotification(entries);
        } else if (selectable == static_cast<swss::Selectable *> (&configDbDeviceMetadataTable)) {
            configDbDeviceMetadataTable.pops(entries);
            processDeviceMetadataNotification(entries);
        }
    }
}

void DHCPMgr::processDeviceMetadataNotification(std::deque<swss::KeyOpFieldsValuesTuple> &entries) {
    //If there is no DHCPv4 Relay config exist then no need to send event for metadata update.
    if (vlansCopy.empty()) {
	return;
    }

    for (auto &entry : entries) {
        std::string key = kfvKey(entry);
        std::vector<swss::FieldValueTuple> fieldValues = kfvFieldsValues(entry);

        if (key != "localhost") {
             continue;
        }

	relay_config *device_data = nullptr;
        try {
            device_data = new relay_config();
        } catch (const std::bad_alloc &e) {
               syslog(LOG_ERR, "[DHCPV4_RELAY] Memory allocation failed: %s", e.what());
               return;
        }

        for (auto &field : fieldValues) {
             std::string f = fvField(field);
             std::string v = fvValue(field);

            if (f == "hostname") {
                device_data->hostname = v;
            } else if (f == "mac") {
                 std::array<uint8_t, MAC_ADDR_LEN> host_mac_addr;
                 string_to_mac_addr(v, host_mac_addr);
                 std::copy(host_mac_addr.begin(), host_mac_addr.end(), device_data->host_mac_addr);
            }
        }

        //Sending sonic as default hostname if it is not present in metadata
        if (device_data->hostname.length() == 0) {
            device_data->hostname ="sonic";
        }

        event_config metadata_event;
        metadata_event.type = DHCPv4_RELAY_METADATA_UPDATE;
        metadata_event.msg = static_cast<void *>(device_data);
	// Write event to config pipe
        if (write(config_pipe[1], &metadata_event, sizeof(metadata_event)) == -1) {
            syslog(LOG_ERR, "[DHCPV4_RELAY] Failed to write metadata update event to pipe: %s", strerror(errno));
            delete device_data;
        }
    }
}

void DHCPMgr::processInterfaceNotification(std::deque<swss::KeyOpFieldsValuesTuple> &entries) {
    for (auto &entry: entries) {
        std::string key = kfvKey(entry);
	std::string operation = kfvOp(entry);

	size_t found = key.find("|");

	std::string intf_name;
	std::string ip_with_mask;
	std::string ip;
        if (found != std::string::npos) {
            intf_name = key.substr(0, found);
            ip_with_mask = key.substr(found+1);
	    ip = ip_with_mask.substr(0, ip_with_mask.find('/'));
        } else {
            continue;
        }

	//Check the source interface is configured in dhcp relay config.
	for (auto &vlan: vlansCopy) {
	    if (vlan.second.source_interface == intf_name) {
		relay_config *relay_msg = nullptr;
                try {
                     relay_msg = new relay_config();
                } catch (const std::bad_alloc &e) {
                     syslog(LOG_ERR, "[DHCPV4_RELAY] Memory allocation failed: %s", e.what());
                     return;  
                }

               relay_msg->vlan = vlan.second.vlan;
	       if (operation == "SET") {
		   relay_msg->is_add = true;
                   if (inet_pton(AF_INET, ip.c_str(), &relay_msg->src_intf_sel_addr.sin_addr) != 1) {
                       syslog(LOG_ERR, "[DHCPV4_RELAY] Invalid IP address");
		       delete relay_msg;
                       return;
                    }

                    relay_msg->src_intf_sel_addr.sin_family = AF_INET;
	       } else if (operation == "DEL") {
                     relay_msg->is_add = false;
	       }

                event_config event;
                event.type = DHCPv4_RELAY_INTERFACE_UPDATE;
                event.msg = static_cast<void *> (relay_msg);
               // Write the pointer address to the pipe
               if (write(config_pipe[1], &event, sizeof(event)) == -1) {
                   syslog(LOG_ERR, "[DHCPV4_RELAY] Failed to write to config update pipe: %s", strerror(errno));
                   delete relay_msg;
               }
            }
	}
    }
}

void DHCPMgr::processRelayNotification(std::deque<swss::KeyOpFieldsValuesTuple> &entries) {
    for (auto &entry: entries) {
        std::string vlan = kfvKey(entry);
        std::string operation = kfvOp(entry);
        std::vector<swss::FieldValueTuple> fieldValues = kfvFieldsValues(entry);
        relay_config *relay_msg = nullptr;
        try {
            relay_msg = new relay_config();
        } catch (const std::bad_alloc &e) {
           syslog(LOG_ERR, "[DHCPV4_RELAY] Memory allocation failed: %s", e.what());
           return; 
        }
        
	relay_msg->vlan = vlan;

        if (operation == "SET") {
           relay_msg->is_add = true;
           for (auto &fieldValue: fieldValues) {
               std::string f = fvField(fieldValue);
               std::string v = fvValue(fieldValue);
               if (f == "dhcpv4_servers") {
                  std::stringstream ss(v);
                  while (ss.good()) {
                       std::string substr;
                       getline(ss, substr, ',');
		       relay_msg->servers.push_back(substr);
                  }
               } else if (f == "server_vrf") {
                  relay_msg->vrf = v;
               } else if (f == "source_interface") {
                  relay_msg->source_interface = v;
               } else if (f == "link_selection") {
                  relay_msg->link_selection_opt = v;
               } else if (f == "server_id_override") {
                  relay_msg->server_id_override_opt = v;
               } else if (f == "vrf_selection") {
                  relay_msg->vrf_selection_opt = v;
               } else if (f == "agent_relay_mode") {
                  relay_msg->agent_relay_mode = v;
               }
               syslog(LOG_DEBUG, "[DHCPV4_RELAY] key: %s, Operation: %s, f: %s, v: %s", vlan.c_str(), operation.c_str(), f.c_str(), v.c_str());
           }

	   //Updating the vrf value with default if vrf is not configured.
           if (relay_msg->vrf.length() == 0) {
               relay_msg->vrf = "default";
            }
	    
	    //Update the vlan cache entry
	    vlansCopy[relay_msg->vlan] = *relay_msg;
        } else if (operation == "DEL") {
              syslog(LOG_INFO, "[DHCPV4_RELAY] Received DELETE operation for VLAN %s", vlan.c_str());
              relay_msg->is_add = false;
	      //Remove the vlan cache entry
	      vlansCopy.erase(relay_msg->vlan);
        }

        if (relay_msg->servers.empty() && operation != "DEL") {
            syslog(LOG_WARNING, "[DHCPV4_RELAY] No servers found for VLAN %s, skipping configuration.", vlan.c_str());
            continue;
        }
        syslog(LOG_INFO, "[DHCPV4_RELAY] %s %s relay config\n", operation.c_str(), vlan.c_str());

        event_config event;
        event.type = DHCPv4_RELAY_CONFIG_UPDATE;
        event.msg = static_cast<void *> (relay_msg);

        // Write the pointer address to the pipe
        if (write(config_pipe[1], &event, sizeof(event)) == -1) {
            syslog(LOG_ERR, "[DHCPV4_RELAY] Failed to write to config update pipe: %s", strerror(errno));
	    delete relay_msg;
        }
    }
}

DHCPMgr::~DHCPMgr() {
    stopThread = true;
}
