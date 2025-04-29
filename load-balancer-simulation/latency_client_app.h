#ifndef LATENCY_CLIENT_APP_H
#define LATENCY_CLIENT_APP_H

// NS-3 Includes
#include "ns3/application.h"
#include "ns3/event-id.h"
#include "ns3/ipv4-address.h"
#include "ns3/inet-socket-address.h"
#include "ns3/nstime.h" // For ns3::Time
#include "ns3/ptr.h"

// Standard Library Includes
#include <map>
#include <random> // For std::mt19937_64, std::uniform_int_distribution
#include <string>
#include <vector>
#include <cstdint> // For uint16_t, uint32_t, uint64_t

// Project-Specific Includes
#include "request_response_header.h" // Custom request/response header

namespace ns3 {

// Forward declaration
class Socket;

/**
 * @brief A client application to measure request-response latency over TCP.
 *
 * This application sends requests containing a sequence number and timestamp,
 * encapsulated within a RequestResponseHeader. It listens for responses,
 * matches them using the sequence number, calculates the round-trip latency,
 * and stores these latencies for analysis.
 */
class LatencyClientApp : public Application
{
  public:
    /**
     * @brief Gets the TypeId for this application.
     * @return The object TypeId.
     */
    static TypeId GetTypeId();

    LatencyClientApp();
    virtual ~LatencyClientApp() override;

    /**
     * @brief Sets the remote server's IPv4 address and port.
     * @param ip The IPv4 address of the server.
     * @param port The port number of the server.
     */
    void SetRemote(Ipv4Address ip, uint16_t port);

    /**
     * @brief Sets the remote server's address using an InetSocketAddress.
     * @param address The InetSocketAddress of the server.
     */
    void SetRemote(InetSocketAddress address);

    /**
     * @brief Sets the total number of requests the client should send.
     * A count of 0 means continuous sending until StopApplication is called or simulation ends.
     * @param count The number of requests.
     */
    void SetRequestCount(uint32_t count);

    /**
     * @brief Sets the time interval between sending consecutive requests.
     * @param interval The time interval.
     */
    void SetRequestInterval(Time interval);

    /**
     * @brief Sets the size of the payload for each request packet.
     * @param size The payload size in bytes.
     */
    void SetRequestSize(uint32_t size);

    /**
     * @brief Retrieves the recorded latencies.
     * @return A constant reference to a vector of Time objects representing latencies.
     */
    const std::vector<Time>& GetLatencies() const;

  protected:
    /**
     * @brief Called by the simulation core to dispose of the application's resources.
     */
    virtual void DoDispose() override;

  private:
    /**
     * @brief Called by the simulation core when the application is scheduled to start.
     */
    virtual void StartApplication() override;

    /**
     * @brief Called by the simulation core when the application is scheduled to stop.
     */
    virtual void StopApplication() override;

    /**
     * @brief Creates and configures the underlying TCP socket for communication.
     */
    void SetupSocket();

    /**
     * @brief Initiates a TCP connection to the configured remote server.
     */
    void Connect();

    /**
     * @brief Callback invoked when the TCP connection attempt succeeds.
     * @param socket The connected socket.
     */
    void ConnectionSucceeded(Ptr<Socket> socket);

    /**
     * @brief Callback invoked when the TCP connection attempt fails.
     * @param socket The socket that failed to connect.
     */
    void ConnectionFailed(Ptr<Socket> socket);

    /**
     * @brief Callback invoked when the TCP socket is closed, either locally or by the peer.
     * @param socket The closed socket.
     */
    void HandleClose(Ptr<Socket> socket);

    /**
      * @brief Callback invoked when an error occurs on the TCP socket.
      * @param socket The socket experiencing the error.
      */
    void HandleError(Ptr<Socket> socket);

    /**
     * @brief Callback invoked when data is received on the TCP socket.
     * This method handles parsing responses and calculating latency.
     * @param socket The socket receiving data.
     */
    void HandleRead(Ptr<Socket> socket);

    /**
     * @brief Callback invoked when the socket's send buffer has available space.
     * Primarily for advanced flow control; not heavily used in this simple client.
     * @param socket The socket.
     * @param availableBytes The number of bytes available in the send buffer.
     */
    void HandleSend(Ptr<Socket> socket, uint32_t availableBytes);

    /**
     * @brief Schedules the sending of the next request packet based on the request interval.
     */
    void ScheduleNextRequest();

    /**
     * @brief Constructs and sends a single request packet to the server.
     */
    void SendRequestPacket();

    // Member Variables
    Ptr<Socket> m_socket;            //!< The TCP socket used for communication.
    Ipv4Address m_peerIpv4Address;   //!< IPv4 address of the remote server or load balancer.
    uint16_t m_peerPort;             //!< Port number of the remote server or load balancer.

    uint32_t m_requestSize;          //!< Size of the application payload in request packets (bytes).
    uint32_t m_requestCount;         //!< Total number of requests to send (0 for continuous).
    Time m_requestInterval;          //!< Interval between sending requests.
    EventId m_sendEvent;             //!< Event ID for the next scheduled request send operation.

    uint64_t m_seqCounter;           //!< Sequence number counter for outgoing requests.
    uint32_t m_requestsSent;         //!< Count of requests sent by this client.
    uint32_t m_responsesReceived;    //!< Count of valid responses received by this client.

    bool m_running;                  //!< True if the application is currently active and running.
    bool m_connected;                //!< True if the TCP socket is currently connected to the peer.

    std::map<uint64_t, Time> m_sentTimes; //!< Stores send timestamps keyed by sequence number for latency calculation.
    std::vector<Time> m_latencies;        //!< Stores calculated round-trip times for received responses.
    std::string m_rxBuffer;               //!< Buffer for assembling incoming TCP stream data into messages.

    std::mt19937_64 m_rng;           //!< Mersenne Twister random number generator engine.
    std::uniform_int_distribution<uint64_t> m_dist; //!< Uniform distribution for generating 64-bit L7 identifiers.
};

} // namespace ns3

#endif // LATENCY_CLIENT_APP_H