#ifndef UTILS_H
#define UTILS_H

// NS-3 Includes
#include "ns3/core-module.h"        // For Ptr, Time, Simulator, uint32_t, uint16_t
#include "ns3/network-module.h"     // For NodeContainer, Node, Ipv4Address (via internet-module below)
#include "ns3/internet-module.h"    // For Ipv4, Ipv4Address, Ipv4InterfaceAddress

// Standard Library Includes
#include <string> // For std::string

namespace ns3 {

// --- Simulation Constants ---
// These constants provide default values for various simulation parameters.
// They are defined in utils.cc.

extern const std::string DATA_RATE;   //!< Default data rate for links (e.g., "100Mbps").
extern const std::string DELAY;       //!< Default delay for links (e.g., "10ms").
extern const uint32_t PACKET_SIZE;    //!< Default packet size in bytes.
extern const uint16_t SERVER_PORT;    //!< Default port number for backend server applications.
extern const uint16_t LB_PORT;        //!< Default port number on which the load balancer listens.

// --- Helper Functions ---

/**
 * @brief Retrieves the primary IPv4 address of a specified network interface on a node.
 *
 * @param node A Ptr to the ns3::Node whose IP address is to be retrieved.
 * @param interfaceIndex The index of the network interface on the node.
 * The loopback interface is typically 0. Physical interfaces
 * usually start from 1.
 * @return The Ipv4Address of the specified interface (specifically, the first address
 * configured on that interface).
 * @throws std::runtime_error if the node does not have IPv4 installed,
 * or if the specified interface index is out of bounds.
 */
Ipv4Address GetIpv4Address(Ptr<Node> node, uint32_t interfaceIndex = 1); // Default to first non-loopback

/**
 * @brief Populates the global IPv4 routing tables for all nodes in the simulation.
 * This function is a simple wrapper around Ipv4GlobalRoutingHelper::PopulateRoutingTables().
 */
void SetupRouting();

/**
 * @brief Prints the IPv4 addresses of all interfaces on all nodes within a NodeContainer.
 * Useful for debugging and verifying network configuration.
 * @param nodes The NodeContainer holding the nodes whose IP addresses are to be printed.
 */
void PrintNodeIps(const NodeContainer& nodes); // Pass by const reference

/**
 * @brief Logs a message prefixed with the current simulation time.
 * This is a utility for creating timestamped log entries using NS_LOG_INFO.
 * @param message The message string to log.
 */
void LogSimulationTime(const std::string& message);

} // namespace ns3

#endif // UTILS_H
