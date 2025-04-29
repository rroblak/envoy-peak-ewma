#ifndef LOAD_BALANCER_H
#define LOAD_BALANCER_H

// NS-3 Includes
#include "ns3/application.h"
#include "ns3/event-id.h"        // For EventId (though not directly used in this header, good for context)
#include "ns3/ipv4-address.h"
#include "ns3/inet-socket-address.h"
#include "ns3/nstime.h"          // For ns3::Time
#include "ns3/ptr.h"             // For Ptr<Socket>, Ptr<Packet>
#include "ns3/socket.h"          // For Ptr<Socket> forward declaration resolution & Address
#include "ns3/packet.h"          // For Ptr<Packet> forward declaration resolution

// Standard Library Includes
#include <vector>
#include <map>
#include <string>
#include <list>
#include <utility>  // For std::pair
#include <algorithm> // For std::find_if
#include <cstdint>   // For uint16_t, uint32_t, uint64_t

// Project-Specific Includes
#include "request_response_header.h" // Custom L7 header

namespace ns3 {

// Forward declarations (Socket and Packet are included above, Address is part of ns3/socket.h)

/**
 * @brief Holds information about a backend server, including its address,
 * weight for load balancing, and the current count of active L7 requests.
 */
struct BackendInfo {
    InetSocketAddress address;           //!< Backend server address (IP:Port).
    uint32_t weight;                     //!< Weight assigned for load balancing decisions.
    uint32_t activeRequests;             //!< Count of L7 requests currently active on this backend.

    /**
     * @brief Constructs BackendInfo with a specific address and weight.
     * @param addr The backend server's address.
     * @param w The weight for the backend.
     */
    BackendInfo(InetSocketAddress addr, uint32_t w)
        : address(addr), weight(w), activeRequests(0) {}

    /**
     * @brief Default constructor. Initializes with a default address and weight.
     * Required for some standard container operations.
     */
    BackendInfo() : address(Ipv4Address::GetAny(), 0), weight(1), activeRequests(0) {}

    BackendInfo(const BackendInfo& other) = default;

    BackendInfo& operator=(const BackendInfo& other) = default;
};


/**
 * @brief Abstract base class for Layer 7 TCP Load Balancer applications.
 *
 * This class provides the foundational framework for L7 load balancing. It handles:
 * - Accepting incoming TCP connections from clients.
 * - Managing TCP connections to backend servers.
 * - Parsing a custom `RequestResponseHeader` to identify requests and responses.
 * - Forwarding client requests to a chosen backend.
 * - Relaying backend responses back to the appropriate client.
 *
 * Derived classes must implement the specific backend selection logic (`ChooseBackend`)
 * and potentially update their internal state based on request lifecycle events
 * (`RecordBackendLatency`, `NotifyRequestSent`, `NotifyRequestFinished`).
 */
class LoadBalancerApp : public Application
{
  public:
    /**
     * @brief Gets the TypeId for this application.
     * @return The object TypeId.
     */
    static TypeId GetTypeId(void);

    LoadBalancerApp();
    virtual ~LoadBalancerApp() override;

    /**
     * @brief Configures the set of backend servers.
     * This replaces any existing backend configuration.
     * @param backends A vector of pairs, where each pair contains the
     * backend's InetSocketAddress and its assigned weight.
     */
    virtual void SetBackends(const std::vector<std::pair<InetSocketAddress, uint32_t>>& backends);

    /**
     * @brief Adds a single backend server or updates its weight if it already exists.
     * @param backendAddress The address (IP:Port) of the backend server.
     * @param weight The weight for this backend. Non-zero is recommended for weighted algorithms.
     */
    virtual void AddBackend(const InetSocketAddress& backendAddress, uint32_t weight);

    /**
     * @brief Adds a single backend server with a default weight of 1.
     * @param backendAddress The address (IP:Port) of the backend server.
     */
    virtual void AddBackend(const InetSocketAddress& backendAddress);

    /**
     * @brief Retrieves the current list of configured backend servers and their information.
     * @return A constant reference to the vector of BackendInfo objects.
     */
    const std::vector<BackendInfo>& GetBackends() const {
        return m_backends;
    }

  protected:
    /**
     * @brief Called by the simulation core to dispose of the application's resources.
     * Ensures all sockets are closed and state is cleaned up.
     */
    virtual void DoDispose(void) override;

    /**
     * @brief Pure virtual method for choosing a backend server.
     *
     * Derived classes must implement their specific load balancing algorithm here.
     * This method is invoked when a new request needs to be routed to a backend.
     *
     * @param packet The request packet. Can be inspected for header fields if needed by the algorithm.
     * @param fromAddress The original client's full address (ns3::Address object).
     * @param l7Identifier The Layer 7 identifier extracted from the request header (e.g., for session persistence).
     * @param[out] chosenBackend The InetSocketAddress of the backend server selected by the algorithm.
     * @return True if a backend was successfully chosen, false otherwise (e.g., no backends available or suitable).
     */
    virtual bool ChooseBackend(Ptr<Packet> packet,
                               const Address& fromAddress,
                               uint64_t l7Identifier,
                               InetSocketAddress& chosenBackend) = 0;

    /**
     * @brief Pure virtual method to record the measured RTT for a request to a specific backend.
     *
     * Derived classes (e.g., latency-aware algorithms like PeakEWMA) can override this
     * to update their internal metrics based on observed latencies.
     *
     * @param backendAddress The address of the backend server for which latency is recorded.
     * @param rtt The measured round-trip time for a request-response cycle to that backend.
     */
    virtual void RecordBackendLatency(InetSocketAddress backendAddress, Time rtt) = 0;

    /**
     * @brief Pure virtual method called when a request has been successfully sent (or queued for sending
     * if a new backend connection is being established) to a backend server.
     *
     * Derived classes should override this to update their internal state, typically by
     * incrementing the active request counter for the specified backend.
     *
     * @param backendAddress The address of the backend server to which the request was sent.
     */
    virtual void NotifyRequestSent(InetSocketAddress backendAddress) = 0;

    /**
     * @brief Pure virtual method called when a request previously sent to a backend has finished.
     * A request is considered finished if a response is received or an error occurs preventing a response.
     *
     * Derived classes should override this to update their internal state, typically by
     * decrementing the active request counter for the specified backend.
     *
     * @param backendAddress The address of the backend server whose request has finished.
     */
    virtual void NotifyRequestFinished(InetSocketAddress backendAddress) = 0;

    // Member Variables accessible by derived classes
    uint16_t m_port;                         //!< Port number on which the load balancer listens.
    std::vector<BackendInfo> m_backends;     //!< List of backend server information structures.

    /**
     * @brief Finds BackendInfo for a given address (non-const version).
     * @param address The backend address to search for.
     * @return Pointer to the mutable BackendInfo if found, nullptr otherwise.
     */
    BackendInfo* FindBackendInfo(InetSocketAddress address) {
        auto it = std::find_if(m_backends.begin(), m_backends.end(),
                               [&address](const BackendInfo& info){ return info.address == address; });
        return (it != m_backends.end()) ? &(*it) : nullptr;
    }

    /**
     * @brief Finds BackendInfo for a given address (const version).
     * @param address The backend address to search for.
     * @return Pointer to the constant BackendInfo if found, nullptr otherwise.
     */
    const BackendInfo* FindBackendInfo(const InetSocketAddress& address) const {
        auto it = std::find_if(m_backends.cbegin(), m_backends.cend(),
                               [&address](const BackendInfo& info){ return info.address == address; });
        return (it != m_backends.cend()) ? &(*it) : nullptr;
    }

  private:
    // Application lifecycle overrides
    virtual void StartApplication(void) override;
    virtual void StopApplication(void) override;

    /**
     * @brief Holds state for a request that is waiting for a new backend connection to be established.
     */
    struct PendingRequest {
        Ptr<Socket> clientSocket;           //!< The originating client socket.
        Ptr<Packet> requestPacket;          //!< The request packet to send once connected.
        Address clientAddress;              //!< Original client address (for context/logging).
        InetSocketAddress targetBackendAddress; //!< The backend chosen for this pending request.
    };

    Ptr<Socket> m_listeningSocket; //!< Socket listening for incoming client TCP connections.

    // --- State Maps for L7 TCP Proxying ---
    // Key: Client Socket, Value: Associated receive buffer for data from this client.
    std::map<Ptr<Socket>, std::string> m_clientRxBuffers;

    // Key: Backend Socket, Value: Associated receive buffer for data from this backend.
    std::map<Ptr<Socket>, std::string> m_backendRxBuffers;

    // Tracks which backend sockets are associated with which client socket.
    // Key: Client Socket, Value: Map of <Backend Address, Backend Socket Ptr> for this client.
    // This allows a client to have connections to multiple backends if the LB logic dictates (e.g. retries to different backends).
    std::map<Ptr<Socket>, std::map<InetSocketAddress, Ptr<Socket>>> m_clientBackendSockets;

    // Reverse mapping: tracks which client socket a backend socket is serving.
    // Key: Backend Socket, Value: Client Socket Ptr it's associated with.
    std::map<Ptr<Socket>, Ptr<Socket>> m_backendClientMap;

    // Stores requests that are pending the establishment of a new backend connection.
    // Key: Backend Socket (the one being connected), Value: PendingRequest details.
    std::map<Ptr<Socket>, PendingRequest> m_pendingBackendRequests;

    // Stores send timestamps for requests to backends, used for RTT calculation.
    // Key: Pair of <Backend Socket Ptr, Request Sequence Number>, Value: Time request was sent.
    using RequestKey = std::pair<Ptr<Socket>, uint32_t>; // Assuming sequence number is uint32_t
    std::map<RequestKey, Time> m_requestSendTimes;

    // --- TCP Callback Handlers ---
    void HandleAccept(Ptr<Socket> socket, const Address& from);

    void HandleClientRead(Ptr<Socket> socket);
    void HandleClientClose(Ptr<Socket> socket);
    void HandleClientError(Ptr<Socket> socket);

    void HandleBackendRead(Ptr<Socket> socket);
    void HandleBackendClose(Ptr<Socket> socket);
    void HandleBackendError(Ptr<Socket> socket);
    void HandleBackendConnectSuccess(Ptr<Socket> socket);
    void HandleBackendConnectFail(Ptr<Socket> socket);

    // Generic send buffer handler, used for both client and backend sockets for flow control.
    void HandleSend(Ptr<Socket> socket, uint32_t availableBytes);

    // --- Core L7 Proxy Logic ---
    void AttemptForwardRequest(Ptr<Socket> clientSocket, Ptr<Packet> requestPacket, const Address& clientAddress);
    void SendToClient(Ptr<Socket> clientSocket, Ptr<Packet> responsePacket);
    void SendToBackend(Ptr<Socket> backendSocket, Ptr<Packet> requestPacket);

    // --- Connection and State Cleanup ---
    void CleanupClient(Ptr<Socket> clientSocket);
    /**
     * @brief Cleans up all state associated with a specific backend socket connection.
     * @param backendSocket The backend socket to clean up.
     * @param mapEraseOnly If true, only removes the socket from internal tracking maps but does not
     * attempt to close the socket itself. This is used for cleaning up stale
     * map entries where the socket might have already been closed or is invalid.
     */
    void CleanupBackendSocket(Ptr<Socket> backendSocket, bool mapEraseOnly = false);
};

} // namespace ns3

#endif /* LOAD_BALANCER_H */
