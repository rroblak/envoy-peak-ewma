#ifndef LATENCY_SERVER_APP_H
#define LATENCY_SERVER_APP_H

// NS-3 Includes
#include "ns3/application.h"
#include "ns3/inet-socket-address.h" 
#include "ns3/nstime.h"              
#include "ns3/ptr.h"

// Standard Library Includes
#include <list>   
#include <map>    
#include <string> 
#include <cstdint> 

// Project-Specific Includes
#include "request_response_header.h" 

namespace ns3 {

// Forward declaration
class Socket;
class Address; 

/**
 * @brief A server application that receives requests and sends responses for latency measurement.
 *
 * This TCP server listens for incoming connections. For each connected client,
 * it reads requests formatted with a RequestResponseHeader, simulates an optional
 * processing delay, and then sends a response back. The response typically echoes
 * the header information from the request, with a payload size of zero.
 * It tracks the total number of requests received.
 */
class LatencyServerApp : public Application
{
  public:
    /**
     * @brief Gets the TypeId for this application.
     * @return The object TypeId.
     */
    static TypeId GetTypeId();

    LatencyServerApp();
    virtual ~LatencyServerApp() override;

    /**
     * @brief Sets the simulated processing delay for each incoming request.
     * @param delay The processing delay time.
     */
    void SetProcessingDelay(Time delay);

    /**
     * @brief Retrieves the total number of requests processed by this server instance.
     * @return The total count of processed requests.
     */
    uint64_t GetTotalRequestsReceived() const;

  protected:
    /**
     * @brief Called by the simulation core to dispose of the application's resources.
     */
    virtual void DoDispose() override;

  private:
    /**
     * @brief Called by the simulation core when the application is scheduled to start.
     * Initializes the listening socket.
     */
    virtual void StartApplication() override;

    /**
     * @brief Called by the simulation core when the application is scheduled to stop.
     * Closes all active sockets.
     */
    virtual void StopApplication() override;

    /**
     * @brief Callback invoked when a new client attempts to connect to the listening socket.
     * @param newSocket The newly accepted socket representing the client connection.
     * @param from The address of the connecting client.
     */
    void HandleAccept(Ptr<Socket> newSocket, const Address& from);

    /**
     * @brief Callback invoked when a connected client's socket is closed.
     * @param socket The client socket that was closed.
     */
    void HandleClientClose(Ptr<Socket> socket);

    /**
     * @brief Callback invoked when an error occurs on a connected client's socket.
     * @param socket The client socket experiencing the error.
     */
    void HandleClientError(Ptr<Socket> socket);

    /**
     * @brief Callback invoked when data is received from a connected client.
     * Manages stream reassembly and triggers request processing.
     * @param socket The socket on which data was received.
     */
    void HandleRead(Ptr<Socket> socket);

    /**
     * @brief Processes a fully assembled request received from a client.
     * @param socket The client socket from which the request originated.
     * @param header The deserialized RequestResponseHeader from the request.
     * @param payloadSize The size of the payload that accompanied the header (may not be used by server logic).
     */
    void ProcessRequest(Ptr<Socket> socket, RequestResponseHeader header, uint32_t payloadSize);

    /**
     * @brief Sends a response packet back to the client.
     * The response contains the echoed header with a zero-sized payload.
     * @param socket The client socket to send the response to.
     * @param header The header to include in the response (typically echoed from the request).
     */
    void SendResponse(Ptr<Socket> socket, RequestResponseHeader header);

    // Member Variables
    uint16_t m_port;                     //!< Port number on which the server listens.
    Ptr<Socket> m_listeningSocket;       //!< The main listening socket for incoming connections.
    std::list<Ptr<Socket>> m_socketList; //!< List of currently active client connection sockets.

    Time m_processingDelay;              //!< Configurable delay to simulate server processing time.

    // Per-client receive buffer to handle TCP stream reassembly.
    std::map<Ptr<Socket>, std::string> m_rxBuffers;

    uint64_t m_requestsReceived = 0;     //!< Counter for the total number of requests processed.
};

} // namespace ns3

#endif // LATENCY_SERVER_APP_H
