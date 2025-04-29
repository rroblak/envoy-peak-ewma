#include "utils.h"

#include "ns3/log.h"
#include "ns3/ipv4.h"                       // For Ptr<Ipv4>
#include "ns3/ipv4-interface-address.h"     // For Ipv4InterfaceAddress
#include "ns3/ipv4-global-routing-helper.h" // For PopulateRoutingTables
#include "ns3/node.h"                       // For Ptr<Node>
#include "ns3/node-container.h"             // For NodeContainer
#include "ns3/simulator.h"                  // For Simulator::Now()

#include <stdexcept> // For std::runtime_error
#include <sstream>   // For std::stringstream

namespace ns3 {

NS_LOG_COMPONENT_DEFINE("SimulationUtils");

// --- Constants Definition ---
const std::string DATA_RATE = "100Mbps";
const std::string DELAY = "10ms"; // Example delay, adjust as needed
const uint32_t PACKET_SIZE = 1024; // Bytes
const uint16_t SERVER_PORT = 9;    // Default Echo port, often used for simple servers
const uint16_t LB_PORT = 80;       // Standard HTTP port, common for load balancers

// --- Helper Functions Implementation ---

Ipv4Address GetIpv4Address(Ptr<Node> node, uint32_t interfaceIndex)
{
    NS_LOG_FUNCTION(node << interfaceIndex);
    if (!node)
    {
        throw std::runtime_error("GetIpv4Address: Provided node is null.");
    }

    Ptr<Ipv4> ipv4 = node->GetObject<Ipv4>();
    if (!ipv4)
    {
        std::ostringstream oss;
        oss << "GetIpv4Address: Node " << node->GetId() << " does not have IPv4 protocol installed.";
        NS_LOG_ERROR(oss.str());
        throw std::runtime_error(oss.str());
    }

    if (interfaceIndex >= ipv4->GetNInterfaces())
    {
        std::ostringstream oss;
        oss << "GetIpv4Address: Node " << node->GetId() << " does not have interface with index "
            << interfaceIndex << ". Available interfaces: " << ipv4->GetNInterfaces() << ".";
        NS_LOG_ERROR(oss.str());
        throw std::runtime_error(oss.str());
    }

    if (ipv4->GetNAddresses(interfaceIndex) == 0)
    {
        std::ostringstream oss;
        oss << "GetIpv4Address: Node " << node->GetId() << ", interface " << interfaceIndex
            << " has no IPv4 addresses configured.";
        NS_LOG_ERROR(oss.str());
        throw std::runtime_error(oss.str());
    }

    // Assuming the first IP address (index 0) on the specified interface is desired.
    Ipv4InterfaceAddress interfaceAddress = ipv4->GetAddress(interfaceIndex, 0);
    Ipv4Address ipv4Addr = interfaceAddress.GetLocal();
    NS_LOG_DEBUG("Node " << node->GetId() << " Interface " << interfaceIndex << " -> IP: " << ipv4Addr);
    return ipv4Addr;
}

void SetupRouting()
{
    NS_LOG_FUNCTION_NOARGS(); // Use NOARGS if function takes no arguments
    NS_LOG_INFO("Populating Global IPv4 Routing Tables...");
    Ipv4GlobalRoutingHelper::PopulateRoutingTables();
    NS_LOG_INFO("Global IPv4 Routing tables populated.");
}

void PrintNodeIps(const NodeContainer& nodes) // Pass by const reference
{
    NS_LOG_FUNCTION(&nodes); // Log address of container for context
    NS_LOG_INFO("--- Node IP Addresses ---");
    for (uint32_t i = 0; i < nodes.GetN(); ++i)
    {
        Ptr<Node> node = nodes.Get(i);
        if (!node) continue; // Should not happen with NodeContainer

        Ptr<Ipv4> ipv4 = node->GetObject<Ipv4>();
        if (ipv4)
        {
            NS_LOG_INFO("Node " << node->GetId() << ":");
            for (uint32_t ifIdx = 0; ifIdx < ipv4->GetNInterfaces(); ++ifIdx)
            {
                if (ipv4->GetNAddresses(ifIdx) > 0) {
                    // Loop through all addresses on an interface if needed, typically one primary.
                    for (uint32_t addrIdx = 0; addrIdx < ipv4->GetNAddresses(ifIdx); ++addrIdx) {
                        Ipv4InterfaceAddress addrInfo = ipv4->GetAddress(ifIdx, addrIdx);
                        Ipv4Address addr = addrInfo.GetLocal();
                        Ipv4Mask mask = addrInfo.GetMask();
                        NS_LOG_INFO("  Interface " << ifIdx << " Address " << addrIdx 
                                      << ": IP " << addr << " Mask " << mask);
                    }
                } else {
                    NS_LOG_INFO("  Interface " << ifIdx << ": No IPv4 Addresses");
                }
            }
        }
        else
        {
            NS_LOG_INFO("Node " << node->GetId() << ": No IPv4 protocol installed.");
        }
    }
    NS_LOG_INFO("-------------------------");
}

void LogSimulationTime(const std::string& message) {
    // This function directly logs the message prefixed with the current simulation time.
    // No scheduling is involved; it logs immediately when called.
    NS_LOG_INFO(Simulator::Now().GetSeconds() << "s - " << message);
}

} // namespace ns3
