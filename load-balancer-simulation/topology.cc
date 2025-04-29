#include "topology.h"

#include "utils.h" // For constants like DATA_RATE, DELAY (if used) and logging context
#include "ns3/log.h"
#include "ns3/csma-helper.h"
#include "ns3/internet-stack-helper.h" // Explicit include for InternetStackHelper
#include "ns3/ipv4-address-helper.h"   // Explicit include for Ipv4AddressHelper
#include "ns3/node-container.h"
#include "ns3/net-device-container.h"
#include "ns3/ipv4-interface-container.h"

#include <sstream> // For std::stringstream (though not used in this cleaned version)

namespace ns3 {

NS_LOG_COMPONENT_DEFINE("TopologyCreator");

void CreateTopology(uint32_t numClients,
                    uint32_t numServers,
                    NodeContainer& clientNodes, // Output parameter
                    Ptr<Node>& lbNode,          // Output parameter
                    NodeContainer& serverNodes, // Output parameter
                    InternetStackHelper& internetStack)
{
    NS_LOG_FUNCTION(numClients << numServers); // Log input parameters
    NS_LOG_INFO("Creating CSMA topology: " << numClients << " client(s) --- LB --- " << numServers << " server(s).");

    // --- 1. Create Nodes ---
    clientNodes.Create(numClients);

    NodeContainer lbNodesContainer; // Temporary container to get a Ptr<Node>
    lbNodesContainer.Create(1);
    lbNode = lbNodesContainer.Get(0); // Assign the single node to the output Ptr

    serverNodes.Create(numServers);

    NS_LOG_INFO("Nodes created: " << clientNodes.GetN() << " clients, 1 Load Balancer, " << serverNodes.GetN() << " servers.");

    // --- 2. Install Internet Stack ---
    // It's generally recommended to install the stack before creating and attaching NetDevices
    // to ensure consistent interface numbering (e.g., Loopback at index 0).
    NS_LOG_INFO("Installing Internet stack on all nodes...");
    internetStack.Install(clientNodes);
    internetStack.Install(lbNode);
    internetStack.Install(serverNodes);
    NS_LOG_INFO("Internet stack installation complete.");

    // --- 3. Configure CSMA Channels and Devices ---
    CsmaHelper csmaHelper;
    // Default CSMA attributes (DataRate="100Mbps", Delay="6560ns") are often sufficient.
    // Customization example (if DATA_RATE and DELAY constants were defined in utils.h):
    // csmaHelper.SetChannelAttribute("DataRate", StringValue(DATA_RATE));
    // csmaHelper.SetChannelAttribute("Delay", TimeValue(Seconds(DELAY)));

    // --- 3a. Frontend Network (Clients <-> Load Balancer) ---
    NS_LOG_INFO("Creating frontend CSMA network (Clients <-> LB)...");
    NodeContainer frontendLinkNodes;
    frontendLinkNodes.Add(lbNode);         // LB is node 0 on this link's container
    frontendLinkNodes.Add(clientNodes);    // Clients are nodes 1 to N on this link's container
    NetDeviceContainer frontendDevices = csmaHelper.Install(frontendLinkNodes);
    // Interface indexing on nodes (assuming loopback is ifIndex 0):
    // - lbNode's frontend NetDevice: ifIndex 1
    // - clientNodes.Get(i)'s NetDevice: ifIndex 1

    // --- 3b. Backend Network (Load Balancer <-> Servers) ---
    NS_LOG_INFO("Creating backend CSMA network (LB <-> Servers)...");
    NodeContainer backendLinkNodes;
    backendLinkNodes.Add(lbNode);          // LB is node 0 on this link's container
    backendLinkNodes.Add(serverNodes);     // Servers are nodes 1 to M on this link's container
    NetDeviceContainer backendDevices = csmaHelper.Install(backendLinkNodes);
    // Interface indexing on nodes:
    // - lbNode's backend NetDevice: ifIndex 2 (since frontend was ifIndex 1)
    // - serverNodes.Get(j)'s NetDevice: ifIndex 1

    // --- 4. Assign IP Addresses ---
    NS_LOG_INFO("Assigning IP addresses...");
    Ipv4AddressHelper addressHelper;

    // Frontend Network (e.g., 192.168.1.0/24)
    // The LB's frontend interface will be .1, clients will be .2, .3, ...
    addressHelper.SetBase("192.168.1.0", "255.255.255.0");
    Ipv4InterfaceContainer frontendInterfaces = addressHelper.Assign(frontendDevices);
    NS_LOG_INFO("  Frontend Network (192.168.1.0/24) IPs assigned.");
    NS_LOG_INFO("    LB VIP (on its ifIndex 1): " << frontendInterfaces.GetAddress(0)); // lbNode was index 0 in frontendLinkNodes
    for (uint32_t i = 0; i < clientNodes.GetN(); ++i) {
        NS_LOG_DEBUG("    Client " << i << " IP (on its ifIndex 1): " << frontendInterfaces.GetAddress(i + 1));
    }

    // Backend Network (e.g., 10.1.1.0/24)
    // The LB's backend interface will be .1, servers will be .2, .3, ...
    addressHelper.SetBase("10.1.1.0", "255.255.255.0");
    Ipv4InterfaceContainer backendInterfaces = addressHelper.Assign(backendDevices);
    NS_LOG_INFO("  Backend Network (10.1.1.0/24) IPs assigned.");
    NS_LOG_INFO("    LB Internal IP (on its ifIndex 2): " << backendInterfaces.GetAddress(0)); // lbNode was index 0 in backendLinkNodes
    for (uint32_t i = 0; i < serverNodes.GetN(); ++i) {
        NS_LOG_DEBUG("    Server " << i << " IP (on its ifIndex 1): " << backendInterfaces.GetAddress(i + 1));
    }

    NS_LOG_INFO("IP address assignment complete.");
    NS_LOG_INFO("Topology creation finished.");
    // Note: Global routing (e.g., Ipv4GlobalRoutingHelper::PopulateRoutingTables())
    // needs to be called in the main simulation script after topology creation.
}

} // namespace ns3
