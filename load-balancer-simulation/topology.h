#ifndef TOPOLOGY_H
#define TOPOLOGY_H

// NS-3 Includes
#include "ns3/core-module.h"        // For Ptr, NodeContainer, uint32_t
#include "ns3/network-module.h"     // For NodeContainer, NetDeviceContainer (implicitly)
#include "ns3/internet-module.h"    // For InternetStackHelper, Ipv4AddressHelper (implicitly)
#include "ns3/csma-module.h"        // For CsmaHelper (used in .cc)

// Standard Library Includes (none strictly needed in this header)

namespace ns3 {

/**
 * @brief Creates a network topology consisting of clients, a load balancer, and servers.
 *
 * The topology is structured as follows:
 * Clients --- CSMA LAN (Frontend Network) --- Load Balancer --- CSMA LAN (Backend Network) --- Servers
 *
 * This function handles node creation, internet stack installation, CSMA device/channel setup,
 * and IP address assignment for all nodes and interfaces.
 *
 * @param numClients The number of client nodes to create.
 * @param numServers The number of backend server nodes to create.
 * @param[out] clientNodes A NodeContainer that will be populated with the created client nodes.
 * @param[out] lbNode A Ptr<Node> that will point to the created load balancer node.
 * @param[out] serverNodes A NodeContainer that will be populated with the created server nodes.
 * @param internetStack An InternetStackHelper instance used to install the internet stack on all nodes.
 * It is passed by reference as its state might be modified (though typically not in basic installs).
 */
void CreateTopology(uint32_t numClients,
                    uint32_t numServers,
                    NodeContainer& clientNodes,
                    Ptr<Node>& lbNode,
                    NodeContainer& serverNodes,
                    InternetStackHelper& internetStack);

} // namespace ns3

#endif // TOPOLOGY_H
