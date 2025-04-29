#include "load_balancer.h"

#include "utils.h" // For LB_PORT definition
#include "ns3/log.h"
#include "ns3/ipv4-address.h"
#include "ns3/nstime.h"
#include "ns3/inet-socket-address.h"
#include "ns3/inet6-socket-address.h" // For GetPeerNameString if IPv6 is possible
#include "ns3/socket.h"
#include "ns3/simulator.h"
#include "ns3/socket-factory.h"
#include "ns3/packet.h"
#include "ns3/uinteger.h"
#include "ns3/tcp-socket-factory.h"
#include "request_response_header.h" // Custom L7 header

#include <sstream>   // For std::ostringstream
#include <algorithm> // For std::find_if
#include <vector>
#include <map>
#include <list>
#include <utility>   // For std::pair, std::move
#include <string>
#include <cstring>   // For std::strerror
#include <cerrno>    // For errno values (though ns-3 uses its own Socket::SocketErrno)
#include <cstdint>

namespace ns3 {

NS_LOG_COMPONENT_DEFINE("LoadBalancerApp");

namespace { // Anonymous namespace for internal linkage helper functions

/**
 * @brief Helper function to get a string representation of a socket's peer address.
 * Handles both IPv4 and IPv6 addresses.
 * @param socket The socket whose peer name is to be retrieved.
 * @return String representation of the peer address, or an error/status message.
 */
std::string GetPeerNameString(Ptr<Socket> socket) {
    if (!socket) {
        return "(null socket)";
    }
    Address from;
    std::ostringstream oss;
    if (socket->GetPeerName(from) == 0) { // Success
        if (InetSocketAddress::IsMatchingType(from)) {
            oss << InetSocketAddress::ConvertFrom(from);
        } else if (Inet6SocketAddress::IsMatchingType(from)) {
            oss << Inet6SocketAddress::ConvertFrom(from);
        } else {
            oss << "(unknown address type)";
        }
    } else {
        oss << "(peer name unavailable)";
    }
    return oss.str();
}

// Helper to convert InetSocketAddress to string for logging
std::string InetAddressToString(const InetSocketAddress& addr) {
    std::ostringstream oss;
    oss << addr;
    return oss.str();
}

} // anonymous namespace


NS_OBJECT_ENSURE_REGISTERED(LoadBalancerApp);

TypeId LoadBalancerApp::GetTypeId()
{
    static TypeId tid = TypeId("ns3::LoadBalancerApp")
                            .SetParent<Application>()
                            .SetGroupName("Applications")
                            .AddAttribute("Port",
                                          "Port on which the load balancer listens for TCP connections.",
                                          UintegerValue(LB_PORT),
                                          MakeUintegerAccessor(&LoadBalancerApp::m_port),
                                          MakeUintegerChecker<uint16_t>());
    return tid;
}

LoadBalancerApp::LoadBalancerApp()
    : m_port(LB_PORT),
      m_listeningSocket(nullptr)
{
    NS_LOG_FUNCTION(this);
}

LoadBalancerApp::~LoadBalancerApp()
{
    NS_LOG_FUNCTION(this);
    m_listeningSocket = nullptr;
}

void LoadBalancerApp::SetBackends(const std::vector<std::pair<InetSocketAddress, uint32_t>>& backends)
{
    NS_LOG_FUNCTION(this);
    m_backends.clear();
    m_backends.reserve(backends.size());

    NS_LOG_INFO("LB (L7 TCP): Setting " << backends.size() << " backends.");
    for(const auto& backendPair : backends) {
        m_backends.emplace_back(backendPair.first, backendPair.second);
        const auto& backendInfo = m_backends.back();

        if (backendInfo.weight == 0) {
            NS_LOG_WARN("LB (L7 TCP): Backend " << backendInfo.address << " added with zero weight. "
                        "This backend might not be selected by some load balancing algorithms.");
        }
        NS_LOG_INFO("  Backend Added: " << backendInfo.address
                      << " with Weight: " << backendInfo.weight
                      << " (L7 Active: " << backendInfo.activeRequests << ")");
    }
    // Derived classes might override this to perform additional state updates (e.g., re-calculating routing tables).
}

void LoadBalancerApp::AddBackend(const InetSocketAddress& backendAddress, uint32_t weight)
{
    NS_LOG_FUNCTION(this << backendAddress << weight);
    uint32_t effectiveWeight = weight; // Use the provided weight directly
    if (weight == 0) {
        NS_LOG_WARN("LB (L7 TCP): Adding backend " << backendAddress << " with zero weight. "
                    "This backend might not be selected by some load balancing algorithms.");
    }

    BackendInfo* existingBackend = FindBackendInfo(backendAddress); // Uses inline helper from .h

    if (existingBackend == nullptr) {
        m_backends.emplace_back(backendAddress, effectiveWeight);
        NS_LOG_INFO("LB (L7 TCP): Added new backend " << backendAddress
                      << " with Weight: " << effectiveWeight << " (L7 Active: 0)");
    } else {
        NS_LOG_INFO("LB (L7 TCP): Backend " << backendAddress
                      << " already exists. Updating weight from " << existingBackend->weight
                      << " to " << effectiveWeight
                      << " (Current L7 Active: " << existingBackend->activeRequests << ")");
        existingBackend->weight = effectiveWeight;
        // Active request count is not reset upon weight change.
    }
}

void LoadBalancerApp::AddBackend(const InetSocketAddress& backendAddress) {
    AddBackend(backendAddress, 1); // Default weight of 1
}

void LoadBalancerApp::StartApplication()
{
    NS_LOG_FUNCTION(this);
    Ptr<Node> node = GetNode();
    NS_LOG_INFO("LB App (L7 TCP) starting on Node " << node->GetId() << " at " << Simulator::Now().GetSeconds() << "s");

    if (m_listeningSocket == nullptr)
    {
        m_listeningSocket = Socket::CreateSocket(node, TcpSocketFactory::GetTypeId());
        if (!m_listeningSocket) {
            NS_FATAL_ERROR("LoadBalancerApp (L7 TCP) Node " << GetNode()->GetId() << ": Failed to create listening socket.");
        }

        InetSocketAddress localAddress(Ipv4Address::GetAny(), m_port);
        if (m_listeningSocket->Bind(localAddress) != 0) {
            NS_FATAL_ERROR("LoadBalancerApp (L7 TCP) Node " << GetNode()->GetId()
                           << ": Failed to bind listening socket to " << localAddress
                           << ". Errno: " << m_listeningSocket->GetErrno()
                           << " (" << std::strerror(m_listeningSocket->GetErrno()) << ")");
        }
        if (m_listeningSocket->Listen() != 0) {
            NS_FATAL_ERROR("LoadBalancerApp (L7 TCP) Node " << GetNode()->GetId()
                           << ": Failed to listen on socket " << localAddress
                           << ". Errno: " << m_listeningSocket->GetErrno()
                           << " (" << std::strerror(m_listeningSocket->GetErrno()) << ")");
        }
        m_listeningSocket->SetAcceptCallback(
            MakeNullCallback<bool, Ptr<Socket>, const Address&>(),
            MakeCallback(&LoadBalancerApp::HandleAccept, this));

        NS_LOG_INFO("LB (L7 TCP) Node " << GetNode()->GetId() << ": Listening on " << localAddress);
    } else {
        NS_LOG_WARN("LB App (L7 TCP) Node " << GetNode()->GetId() << ": StartApplication called but listening socket already exists.");
    }

    if (m_backends.empty()) {
        NS_LOG_WARN("LB Warning (L7 TCP) Node " << GetNode()->GetId() << ": Starting with no backend servers configured.");
    }
}

void LoadBalancerApp::StopApplication()
{
    NS_LOG_FUNCTION(this);
    Ptr<Node> node = GetNode();
    NS_LOG_INFO("LB App (L7 TCP) stopping on Node " << node->GetId() << " at " << Simulator::Now().GetSeconds() << "s");

    if (m_listeningSocket)
    {
        NS_LOG_DEBUG("Closing listening socket " << m_listeningSocket);
        m_listeningSocket->Close();
        m_listeningSocket->SetAcceptCallback(MakeNullCallback<bool, Ptr<Socket>, const Address&>(),
                                             MakeNullCallback<void, Ptr<Socket>, const Address&>());
        m_listeningSocket = nullptr;
    }

    NS_LOG_INFO("Closing " << m_clientBackendSockets.size() << " active client connections and their associated backend connections.");
    std::vector<Ptr<Socket>> clientKeysToCleanup;
    for (auto const& [clientSock, backendMap] : m_clientBackendSockets) {
        clientKeysToCleanup.push_back(clientSock);
    }
    for (Ptr<Socket> clientSock : clientKeysToCleanup) {
        CleanupClient(clientSock); // This handles associated backend sockets too
    }
    m_clientBackendSockets.clear(); // Should be empty after CleanupClient calls

    NS_LOG_INFO("Closing " << m_pendingBackendRequests.size() << " pending backend connections.");
    std::vector<Ptr<Socket>> pendingBackendKeysToCleanup;
    for (auto const& [backendSock, pendingInfo] : m_pendingBackendRequests) {
        pendingBackendKeysToCleanup.push_back(backendSock);
    }
    for(Ptr<Socket> backendSock : pendingBackendKeysToCleanup) {
        CleanupBackendSocket(backendSock); // This handles NotifyRequestFinished
    }
    m_pendingBackendRequests.clear(); // Should be empty

    m_clientRxBuffers.clear();
    m_backendRxBuffers.clear();
    m_backendClientMap.clear();
    m_requestSendTimes.clear();

    NS_LOG_INFO("LB App (L7 TCP) on Node " << GetNode()->GetId() << " stopped.");
}

void LoadBalancerApp::DoDispose(void) {
    NS_LOG_FUNCTION(this);
    // Ensure StopApplication logic is run if not already stopped
    if (m_listeningSocket || !m_clientBackendSockets.empty() || !m_pendingBackendRequests.empty()) {
        NS_LOG_DEBUG("DoDispose called while LB App was still active. Calling StopApplication first.");
        StopApplication();
    }
    Application::DoDispose();
}

void LoadBalancerApp::HandleAccept(Ptr<Socket> acceptedSocket, const Address& from)
{
    NS_LOG_FUNCTION(this << acceptedSocket << from);
    InetSocketAddress inetFrom = InetSocketAddress::ConvertFrom(from);
    NS_LOG_INFO("LB (L7 TCP) Node " << GetNode()->GetId() << ": Accepted connection from "
                  << inetFrom << " on socket " << acceptedSocket);

    acceptedSocket->SetRecvCallback(MakeCallback(&LoadBalancerApp::HandleClientRead, this));
    acceptedSocket->SetSendCallback(MakeCallback(&LoadBalancerApp::HandleSend, this));
    acceptedSocket->SetCloseCallbacks(MakeCallback(&LoadBalancerApp::HandleClientClose, this),
                                      MakeCallback(&LoadBalancerApp::HandleClientError, this));

    m_clientRxBuffers.emplace(acceptedSocket, "");
    m_clientBackendSockets.emplace(acceptedSocket, std::map<InetSocketAddress, Ptr<Socket>>());

    NS_LOG_DEBUG("LB (L7 TCP): Initialized state for client socket " << acceptedSocket);
}

void LoadBalancerApp::HandleClientRead(Ptr<Socket> clientSocket)
{
    NS_LOG_FUNCTION(this << clientSocket);
    Ptr<Packet> packet;
    Address clientAddress;
    clientSocket->GetPeerName(clientAddress);

    auto buffer_it = m_clientRxBuffers.find(clientSocket);
    if (buffer_it == m_clientRxBuffers.end()) {
        NS_LOG_DEBUG("LB (L7): Client buffer missing for socket " << clientSocket
                     << " in HandleClientRead (likely closed). Ignoring read.");
        return;
    }
    std::string& currentRxBuffer = buffer_it->second;

    while ((packet = clientSocket->Recv())) {
        if (packet->GetSize() == 0) {
            NS_LOG_INFO("LB (L7): Client " << GetPeerNameString(clientSocket)
                          << " (socket " << clientSocket << ") closed connection gracefully (Recv 0 bytes).");
            // HandleClientClose will be invoked by the socket layer.
            return;
        }

        NS_LOG_DEBUG("LB (L7): Received " << packet->GetSize() << " bytes from client " << clientSocket);
        std::vector<uint8_t> tempBuffer(packet->GetSize());
        packet->CopyData(tempBuffer.data(), tempBuffer.size());
        currentRxBuffer.append(reinterpret_cast<char*>(tempBuffer.data()), tempBuffer.size());
    }

    NS_LOG_DEBUG("LB (L7): Client " << clientSocket << " buffer size after recv loop: " << currentRxBuffer.size());

    uint32_t headerSize = RequestResponseHeader().GetSerializedSize();
    while (currentRxBuffer.size() >= headerSize)
    {
        Ptr<Packet> tempPacket = Create<Packet>(reinterpret_cast<const uint8_t*>(currentRxBuffer.data()), headerSize);
        RequestResponseHeader reqHeader;
        if (tempPacket->PeekHeader(reqHeader) != headerSize) {
            NS_LOG_WARN("LB (L7): Could not peek complete header from client " << clientSocket
                        << " buffer start. Buffer size: " << currentRxBuffer.size() << ". Possible data corruption.");
            break;
        }

        uint32_t expectedPayloadSize = reqHeader.GetPayloadSize();
        uint32_t expectedTotalSize = headerSize + expectedPayloadSize;

        if (currentRxBuffer.size() >= expectedTotalSize)
        {
            NS_LOG_DEBUG("LB (L7): Processing full request Seq=" << reqHeader.GetSeq() << " Size=" << expectedTotalSize
                         << " from client " << clientSocket);

            Ptr<Packet> requestPacketToForward = Create<Packet>(reinterpret_cast<const uint8_t*>(currentRxBuffer.data()), expectedTotalSize);
            currentRxBuffer.erase(0, expectedTotalSize);
            NS_LOG_DEBUG("LB (L7): Consumed " << expectedTotalSize << " bytes from client buffer. Remaining: " << currentRxBuffer.size());

            AttemptForwardRequest(clientSocket, requestPacketToForward, clientAddress);
        }
        else
        {
            NS_LOG_DEBUG("LB (L7): Incomplete request in client buffer. Need " << expectedTotalSize
                         << " (Header:" << headerSize << ", Payload:" << expectedPayloadSize
                         << "), have " << currentRxBuffer.size() << ". Waiting for more data.");
            break;
        }
    }

    Socket::SocketErrno sock_errno = clientSocket->GetErrno();
    if (sock_errno != Socket::ERROR_NOTERROR &&
        sock_errno != Socket::ERROR_AGAIN &&
        sock_errno != Socket::ERROR_SHUTDOWN &&
        sock_errno != Socket::ERROR_NOTCONN) {
        NS_LOG_WARN("LB (L7): Error reading from client " << clientSocket << " (" << GetPeerNameString(clientSocket)
                      << "): Errno " << sock_errno << " (" << std::strerror(sock_errno) << ")");
        CleanupClient(clientSocket);
    }
}

void LoadBalancerApp::AttemptForwardRequest(Ptr<Socket> clientSocket, Ptr<Packet> requestPacket, const Address& clientAddress) {
    NS_LOG_FUNCTION(this << clientSocket << requestPacket << clientAddress);

    InetSocketAddress chosenBackendAddress(Ipv4Address::GetAny(), 0);
    RequestResponseHeader traceHeader;
    requestPacket->PeekHeader(traceHeader);
    uint32_t currentSeq = traceHeader.GetSeq();
    uint64_t l7Identifier = traceHeader.GetL7Identifier();

    std::string clientAddrStr = "(client address unavailable)";
    if (!clientAddress.IsInvalid() && InetSocketAddress::IsMatchingType(clientAddress)) {
        clientAddrStr = InetAddressToString(InetSocketAddress::ConvertFrom(clientAddress));
    }

    bool backendChosen = ChooseBackend(requestPacket, clientAddress, l7Identifier, chosenBackendAddress);

    if (!backendChosen) {
        NS_LOG_WARN("LB (L7): No backend chosen by algorithm for request Seq=" << currentSeq
                      << " from " << clientAddrStr << " (L7Id=" << l7Identifier << "). Dropping request.");
        return;
    }
    NS_LOG_INFO("LB (L7): Request Seq=" << currentSeq << " from " << clientAddrStr << " (L7Id=" << l7Identifier << ")"
                  << " assigned to Backend " << chosenBackendAddress);

    auto client_backends_it = m_clientBackendSockets.find(clientSocket);
    if (client_backends_it == m_clientBackendSockets.end()) {
        NS_LOG_WARN("LB (L7): Client socket " << clientSocket << " not found in state map during forward attempt for Seq="
                      << currentSeq << ". This should not happen if client is active. Dropping request.");
        return;
    }
    std::map<InetSocketAddress, Ptr<Socket>>& backendMap = client_backends_it->second;

    auto backend_sock_it = backendMap.find(chosenBackendAddress);
    Ptr<Socket> backendSocketToUse = nullptr;
    bool reuseConnection = false;

    if (backend_sock_it != backendMap.end()) {
        backendSocketToUse = backend_sock_it->second;
        if (backendSocketToUse && backendSocketToUse->GetErrno() == Socket::ERROR_NOTERROR) {
            reuseConnection = true;
        } else {
            NS_LOG_DEBUG(" -- Found existing entry for backend " << chosenBackendAddress
                         << ", but socket " << backendSocketToUse << " was null or errored (errno="
                         << (backendSocketToUse ? backendSocketToUse->GetErrno() : -1) << "). Will create new one.");
            CleanupBackendSocket(backendSocketToUse, true); 
            backendMap.erase(backend_sock_it);
            backendSocketToUse = nullptr; 
        }
    }

    if (reuseConnection)
    {
        NS_LOG_DEBUG("LB (L7): Reusing existing backend socket " << backendSocketToUse
                     << " for request Seq=" << currentSeq << " to " << chosenBackendAddress);

        NotifyRequestSent(chosenBackendAddress); 
        m_requestSendTimes[{backendSocketToUse, currentSeq}] = Simulator::Now();
        SendToBackend(backendSocketToUse, requestPacket);
    }
    else
    {
        NS_LOG_INFO("LB (L7): No active/usable connection to " << chosenBackendAddress
                      << " for client " << clientSocket << ". Establishing new one for Req Seq=" << currentSeq << ".");

        Ptr<Socket> newBackendSocket = Socket::CreateSocket(GetNode(), TcpSocketFactory::GetTypeId());
        if (!newBackendSocket) {
            NS_LOG_ERROR("LB (L7): Failed to create new backend socket for " << chosenBackendAddress
                         << ". Dropping request Seq=" << currentSeq << ".");
            return;
        }

        NotifyRequestSent(chosenBackendAddress);

        auto emplaceResult = m_pendingBackendRequests.emplace(
            newBackendSocket,
            PendingRequest{clientSocket, requestPacket->Copy(), clientAddress, chosenBackendAddress}
        );

        if (!emplaceResult.second) { 
            NS_LOG_ERROR("LB (L7): Failed to emplace pending request; key (new backend socket "
                         << newBackendSocket << ") already exists. This is unexpected. Dropping request Seq=" << currentSeq);
            NotifyRequestFinished(chosenBackendAddress); 
            newBackendSocket->Close(); 
            return;
        }

        m_backendClientMap[newBackendSocket] = clientSocket;
        backendMap[chosenBackendAddress] = newBackendSocket; 

        newBackendSocket->SetConnectCallback(MakeCallback(&LoadBalancerApp::HandleBackendConnectSuccess, this),
                                             MakeCallback(&LoadBalancerApp::HandleBackendConnectFail, this));
        newBackendSocket->SetCloseCallbacks(MakeCallback(&LoadBalancerApp::HandleBackendClose, this),
                                            MakeCallback(&LoadBalancerApp::HandleBackendError, this));

        NS_LOG_DEBUG("LB (L7): Attempting connection to " << chosenBackendAddress
                     << " using new socket " << newBackendSocket << " for request from " << clientSocket);
        newBackendSocket->Connect(chosenBackendAddress);
    }
}


void LoadBalancerApp::HandleBackendConnectSuccess(Ptr<Socket> backendSocket)
{
    NS_LOG_FUNCTION(this << backendSocket);

    auto pending_it = m_pendingBackendRequests.find(backendSocket);
    if (pending_it == m_pendingBackendRequests.end()) {
        NS_LOG_WARN("LB (L7): Backend socket " << backendSocket << " connected successfully, but no pending request found. "
                    "Client might have closed or request was otherwise cleaned up. Closing this backend socket.");
        CleanupBackendSocket(backendSocket);
        return;
    }

    PendingRequest pendingInfo = std::move(pending_it->second); 
    m_pendingBackendRequests.erase(pending_it); 

    Ptr<Socket> clientSocket = pendingInfo.clientSocket;
    Ptr<Packet> requestPacket = pendingInfo.requestPacket;
    InetSocketAddress backendAddress = pendingInfo.targetBackendAddress;

    NS_LOG_INFO("LB (L7): Backend connection to " << backendAddress << " (socket " << backendSocket
                  << ") succeeded for client " << clientSocket);

    if (!clientSocket || clientSocket->GetErrno() != Socket::ERROR_NOTERROR) {
        NS_LOG_WARN("LB (L7): Client " << clientSocket << " closed or errored before backend " << backendSocket
                      << " (" << backendAddress << ") connected. Closing backend and dropping request.");
        NotifyRequestFinished(backendAddress); 
        CleanupBackendSocket(backendSocket);
        return;
    }

    m_backendRxBuffers.emplace(backendSocket, "");
    NS_LOG_DEBUG("LB (L7): Initialized RX buffer for backend socket " << backendSocket);

    backendSocket->SetRecvCallback(MakeCallback(&LoadBalancerApp::HandleBackendRead, this));
    backendSocket->SetSendCallback(MakeCallback(&LoadBalancerApp::HandleSend, this));

    RequestResponseHeader reqHeader;
    requestPacket->PeekHeader(reqHeader);
    uint32_t currentSeq = reqHeader.GetSeq();
    m_requestSendTimes[{backendSocket, currentSeq}] = Simulator::Now(); 

    SendToBackend(backendSocket, requestPacket);
}


void LoadBalancerApp::HandleBackendConnectFail(Ptr<Socket> backendSocket)
{
    NS_LOG_FUNCTION(this << backendSocket);
    Socket::SocketErrno error = backendSocket->GetErrno();
    std::string backendSocketIdStr; 
    {
        std::ostringstream oss;
        oss << static_cast<void*>(PeekPointer(backendSocket));
        backendSocketIdStr = oss.str();
    }

    InetSocketAddress targetBackendAddress(Ipv4Address::GetAny(), 0); 

    auto pending_it = m_pendingBackendRequests.find(backendSocket);
    if (pending_it != m_pendingBackendRequests.end()) {
        targetBackendAddress = pending_it->second.targetBackendAddress; 
        RequestResponseHeader reqHeader;
        pending_it->second.requestPacket->PeekHeader(reqHeader);
        NS_LOG_WARN("LB (L7): Failed to connect to backend " << targetBackendAddress
                      << " (socket " << backendSocketIdStr << "). Errno: " << error
                      << " (" << std::strerror(error) << "). Dropping request Seq=" << reqHeader.GetSeq());

        NotifyRequestFinished(targetBackendAddress); 
        m_pendingBackendRequests.erase(pending_it);
    } else {
        Address peerAddrAttempt;
        if (backendSocket->GetPeerName(peerAddrAttempt) == 0 && InetSocketAddress::IsMatchingType(peerAddrAttempt)) {
            targetBackendAddress = InetSocketAddress::ConvertFrom(peerAddrAttempt);
             NS_LOG_WARN("LB (L7): Backend socket " << backendSocketIdStr << " (intended for " << targetBackendAddress
                           << ") connection failed (Errno: " << error << "), but no PENDING request found. Assuming already cleaned up.");
        } else {
            NS_LOG_WARN("LB (L7): Backend socket " << backendSocketIdStr
                          << " connection failed (Errno: " << error << "), no pending request or known target address.");
        }
    }
    CleanupBackendSocket(backendSocket); 
}


void LoadBalancerApp::HandleBackendRead(Ptr<Socket> backendSocket)
{
    NS_LOG_FUNCTION(this << backendSocket);
    Ptr<Packet> packet;
    std::string backendAddrStr = GetPeerNameString(backendSocket); 
    InetSocketAddress backendInetAddr(Ipv4Address::GetAny(), 0);
    bool backendAddrResolved = false;
    Address rawBackendAddr;
    if(backendSocket->GetPeerName(rawBackendAddr) == 0 && InetSocketAddress::IsMatchingType(rawBackendAddr)) {
        backendInetAddr = InetSocketAddress::ConvertFrom(rawBackendAddr);
        backendAddrResolved = true;
    }


    auto client_map_it = m_backendClientMap.find(backendSocket);
    if (client_map_it == m_backendClientMap.end()) {
        NS_LOG_DEBUG("LB (L7): Read from backend socket " << backendSocket << " (" << backendAddrStr
                     << ") with no associated client (likely closing). Ignoring read.");
        return;
    }
    Ptr<Socket> clientSocket = client_map_it->second;

    if (!clientSocket || clientSocket->GetErrno() != Socket::ERROR_NOTERROR) {
        NS_LOG_DEBUG("LB (L7): Client socket " << clientSocket << " missing or errored for backend "
                     << backendSocket << " (" << backendAddrStr << "). Cleaning up backend.");
        CleanupBackendSocket(backendSocket);
        return;
    }

    auto buffer_it = m_backendRxBuffers.find(backendSocket);
    if (buffer_it == m_backendRxBuffers.end()) {
        NS_LOG_ERROR("LB (L7): Backend buffer missing for socket " << backendSocket << " (" << backendAddrStr
                     << ") in HandleBackendRead! State inconsistency. Cleaning up.");
        CleanupBackendSocket(backendSocket);
        return;
    }
    std::string& currentRxBuffer = buffer_it->second;

    while ((packet = backendSocket->Recv())) {
        if (packet->GetSize() == 0) {
            NS_LOG_INFO("LB (L7): Backend " << backendSocket << " (" << backendAddrStr
                          << ") closed connection gracefully (Recv 0 bytes).");
            return;
        }
        NS_LOG_DEBUG("LB (L7): Received " << packet->GetSize() << " bytes from backend " << backendSocket << " (" << backendAddrStr << ")");
        std::vector<uint8_t> tempBuffer(packet->GetSize());
        packet->CopyData(tempBuffer.data(), tempBuffer.size());
        currentRxBuffer.append(reinterpret_cast<char*>(tempBuffer.data()), tempBuffer.size());
    }
    NS_LOG_DEBUG("LB (L7): Backend " << backendSocket << " (" << backendAddrStr << ") buffer size after recv loop: " << currentRxBuffer.size());

    uint32_t headerSize = RequestResponseHeader().GetSerializedSize();
    while (currentRxBuffer.size() >= headerSize)
    {
        Ptr<Packet> tempPacket = Create<Packet>(reinterpret_cast<const uint8_t*>(currentRxBuffer.data()), headerSize);
        RequestResponseHeader respHeader;
        if (tempPacket->PeekHeader(respHeader) != headerSize) {
            NS_LOG_WARN("LB (L7): Could not peek complete header from backend " << backendSocket << " (" << backendAddrStr
                        << ") buffer start. Buffer size: " << currentRxBuffer.size() << ". Possible data corruption.");
            break;
        }

        uint32_t expectedPayloadSize = respHeader.GetPayloadSize(); 
        uint32_t expectedTotalSize = headerSize + expectedPayloadSize;

        if (currentRxBuffer.size() >= expectedTotalSize)
        {
            NS_LOG_DEBUG("LB (L7): Processing full response Seq=" << respHeader.GetSeq() << " Size=" << expectedTotalSize
                         << " from backend " << backendSocket << " (" << backendAddrStr << ")");

            Ptr<Packet> packetToForwardToClient = Create<Packet>(reinterpret_cast<const uint8_t*>(currentRxBuffer.data()), expectedTotalSize);
            currentRxBuffer.erase(0, expectedTotalSize);
            NS_LOG_DEBUG("LB (L7): Consumed " << expectedTotalSize << " bytes from backend buffer. Remaining: " << currentRxBuffer.size());

            uint32_t currentSeq = respHeader.GetSeq();
            auto sendTimeIt = m_requestSendTimes.find({backendSocket, currentSeq});

            if (backendAddrResolved && sendTimeIt != m_requestSendTimes.end()) {
                Time sendTime = sendTimeIt->second;
                Time rtt = Simulator::Now() - sendTime;
                NS_LOG_DEBUG("LB (L7): Calculated RTT for Seq=" << currentSeq << " on backend " << backendInetAddr << " is " << rtt);
                RecordBackendLatency(backendInetAddr, rtt);
                m_requestSendTimes.erase(sendTimeIt);
            } else if (!backendAddrResolved) {
                NS_LOG_WARN("LB (L7): Cannot record latency for Seq=" << currentSeq
                              << ", backend address unknown for socket " << backendSocket);
            } else {
                NS_LOG_WARN("LB (L7): Could not find send time for response Seq=" << currentSeq
                              << " from backend " << backendInetAddr << " (socket " << backendSocket << ")");
            }

            if(backendAddrResolved) {
                NotifyRequestFinished(backendInetAddr);
            } else {
                 NS_LOG_WARN("LB (L7): Cannot notify request finished for Seq=" << currentSeq
                               << ", backend address unknown for socket " << backendSocket);
            }

            SendToClient(clientSocket, packetToForwardToClient);
        }
        else
        {
            NS_LOG_DEBUG("LB (L7): Incomplete response in backend buffer. Need " << expectedTotalSize
                         << ", have " << currentRxBuffer.size() << ". Waiting for more data.");
            break;
        }
    }

    Socket::SocketErrno sock_errno = backendSocket->GetErrno();
    if (sock_errno != Socket::ERROR_NOTERROR &&
        sock_errno != Socket::ERROR_AGAIN &&
        sock_errno != Socket::ERROR_SHUTDOWN &&
        sock_errno != Socket::ERROR_NOTCONN) {
        NS_LOG_WARN("LB (L7): Error reading from backend " << backendSocket << " (" << backendAddrStr
                      << "): Errno " << sock_errno << " (" << std::strerror(sock_errno) << ")");
        CleanupBackendSocket(backendSocket);
    }
}


void LoadBalancerApp::SendToClient(Ptr<Socket> clientSocket, Ptr<Packet> responsePacket) {
    NS_LOG_FUNCTION(this << clientSocket << responsePacket);
    if (!clientSocket || clientSocket->GetErrno() != Socket::ERROR_NOTERROR) {
        RequestResponseHeader respHeader;
        responsePacket->PeekHeader(respHeader); 
        NS_LOG_WARN("LB (L7): Attempted to send response Seq=" << respHeader.GetSeq()
                      << " to invalid client socket " << clientSocket
                      << " (Errno=" << (clientSocket ? clientSocket->GetErrno() : -1) << ")");
        return;
    }

    RequestResponseHeader respHeader;
    responsePacket->PeekHeader(respHeader);
    NS_LOG_DEBUG("LB (L7): Forwarding response Seq=" << respHeader.GetSeq() << " (Size=" << responsePacket->GetSize()
                 << ") to client " << clientSocket << " (" << GetPeerNameString(clientSocket) << ")");

    int sentBytes = clientSocket->Send(responsePacket);

    if (sentBytes < 0) {
        Socket::SocketErrno error = clientSocket->GetErrno();
        NS_LOG_WARN("LB (L7): Error sending L7 response Seq=" << respHeader.GetSeq() << " to client "
                      << clientSocket << " (" << GetPeerNameString(clientSocket) << "): Errno " << error
                      << " (" << std::strerror(error) << ")");
    } else if (static_cast<uint32_t>(sentBytes) < responsePacket->GetSize()) {
        NS_LOG_WARN("LB (L7): Could not send full L7 response Seq=" << respHeader.GetSeq() << " to client "
                      << clientSocket << " immediately. Sent " << sentBytes << "/" << responsePacket->GetSize()
                      << ". Disabling reads from associated backend sockets temporarily.");

        auto client_backends_it = m_clientBackendSockets.find(clientSocket);
        if (client_backends_it != m_clientBackendSockets.end()) {
            for (auto const& [addr, backendSock] : client_backends_it->second) {
                if (backendSock && backendSock->GetErrno() == Socket::ERROR_NOTERROR) { 
                    NS_LOG_DEBUG(" -- Disabling read on backend " << backendSock << " for client " << clientSocket);
                    backendSock->SetRecvCallback(MakeNullCallback<void, Ptr<Socket>>());
                }
            }
        }
    } else {
        NS_LOG_DEBUG("LB (L7): Forwarded complete response for Seq=" << respHeader.GetSeq() << " to client " << clientSocket);
    }
}

void LoadBalancerApp::SendToBackend(Ptr<Socket> backendSocket, Ptr<Packet> requestPacket) {
    NS_LOG_FUNCTION(this << backendSocket << requestPacket);
    RequestResponseHeader reqHeader; 
    requestPacket->PeekHeader(reqHeader);
    InetSocketAddress targetBackendAddress(Ipv4Address::GetAny(), 0);
    bool targetAddrKnown = false;
    std::string targetBackendAddressStr = "unknown";

    Address peerAddr;
    if(backendSocket && backendSocket->GetPeerName(peerAddr) == 0 && InetSocketAddress::IsMatchingType(peerAddr)) {
        targetBackendAddress = InetSocketAddress::ConvertFrom(peerAddr);
        targetAddrKnown = true;
        targetBackendAddressStr = InetAddressToString(targetBackendAddress);
    } else { 
        auto pending_it = m_pendingBackendRequests.find(backendSocket);
        if (pending_it != m_pendingBackendRequests.end()) {
            targetBackendAddress = pending_it->second.targetBackendAddress;
            targetAddrKnown = true;
            targetBackendAddressStr = InetAddressToString(targetBackendAddress);
        }
    }


    if (!backendSocket || backendSocket->GetErrno() != Socket::ERROR_NOTERROR) {
        NS_LOG_WARN("LB (L7): Attempted to send request Seq=" << reqHeader.GetSeq()
                      << " to invalid or non-ready backend socket " << backendSocket
                      << " (Target: " << targetBackendAddressStr
                      << ", Errno: " << (backendSocket ? backendSocket->GetErrno() : -1) << ")");
        if (targetAddrKnown) {
            NotifyRequestFinished(targetBackendAddress); 
        }
        CleanupBackendSocket(backendSocket);
        return;
    }

    if (!targetAddrKnown) {
         NS_LOG_WARN("LB (L7): Target backend address unknown for socket " << backendSocket << " during SendToBackend for Seq=" << reqHeader.GetSeq());
    }


    NS_LOG_DEBUG("LB (L7): Forwarding request Seq=" << reqHeader.GetSeq() << " (Size=" << requestPacket->GetSize()
                 << ") to backend " << backendSocket << " (" << GetPeerNameString(backendSocket) << ")");

    int sentBytes = backendSocket->Send(requestPacket);

    if (sentBytes < 0) {
        Socket::SocketErrno error = backendSocket->GetErrno();
        NS_LOG_WARN("LB (L7): Error sending L7 request Seq=" << reqHeader.GetSeq() << " to backend "
                      << backendSocket << " (" << GetPeerNameString(backendSocket) << "): Errno " << error
                      << " (" << std::strerror(error) << ")");
        if (targetAddrKnown) {
            NotifyRequestFinished(targetBackendAddress); 
        }
    } else if (static_cast<uint32_t>(sentBytes) < requestPacket->GetSize()) {
        NS_LOG_WARN("LB (L7): Could not send full L7 request Seq=" << reqHeader.GetSeq() << " to backend "
                      << backendSocket << " immediately. Sent " << sentBytes << "/" << requestPacket->GetSize()
                      << ". Disabling reads from associated client socket temporarily.");
        auto client_it = m_backendClientMap.find(backendSocket);
        if (client_it != m_backendClientMap.end()) {
            Ptr<Socket> clientSocket = client_it->second;
            if (clientSocket && clientSocket->GetErrno() == Socket::ERROR_NOTERROR) {
                NS_LOG_DEBUG(" -- Disabling read on client " << clientSocket << " for backend " << backendSocket);
                clientSocket->SetRecvCallback(MakeNullCallback<void, Ptr<Socket>>());
            }
        }
    } else {
        NS_LOG_DEBUG("LB (L7): Forwarded complete request for Seq=" << reqHeader.GetSeq() << " to backend " << backendSocket);
    }
}

void LoadBalancerApp::HandleSend(Ptr<Socket> socket, uint32_t availableBytes)
{
    NS_LOG_FUNCTION(this << socket << availableBytes);

    auto backend_client_it = m_backendClientMap.find(socket);
    if (backend_client_it != m_backendClientMap.end()) {
        Ptr<Socket> clientSocket = backend_client_it->second;
        if (clientSocket && clientSocket->GetErrno() == Socket::ERROR_NOTERROR) {
            NS_LOG_DEBUG("LB (L7): Backend socket " << socket << " (" << GetPeerNameString(socket)
                         << ") has send space (" << availableBytes << " bytes). Re-enabling read on Client socket " << clientSocket);
            clientSocket->SetRecvCallback(MakeCallback(&LoadBalancerApp::HandleClientRead, this));
            Simulator::ScheduleNow(&LoadBalancerApp::HandleClientRead, this, clientSocket);
        }
        return; 
    }

    auto client_backends_it = m_clientBackendSockets.find(socket);
    if (client_backends_it != m_clientBackendSockets.end()) {
        NS_LOG_DEBUG("LB (L7): Client socket " << socket << " (" << GetPeerNameString(socket)
                     << ") has send space (" << availableBytes << " bytes). Re-enabling reads on associated backend sockets.");
        for (auto const& [addr, backendSock] : client_backends_it->second) {
            if (backendSock && backendSock->GetErrno() == Socket::ERROR_NOTERROR) {
                NS_LOG_DEBUG(" -- Re-enabling read on Backend socket " << backendSock << " (" << GetPeerNameString(backendSock) << ")");
                backendSock->SetRecvCallback(MakeCallback(&LoadBalancerApp::HandleBackendRead, this));
                Simulator::ScheduleNow(&LoadBalancerApp::HandleBackendRead, this, backendSock);
            }
        }
        return; 
    }

    NS_LOG_DEBUG("LB (L7): HandleSend callback for unknown or already closed socket " << socket);
}

void LoadBalancerApp::HandleClientClose(Ptr<Socket> clientSocket)
{
    NS_LOG_FUNCTION(this << clientSocket);
    NS_LOG_INFO("LB (L7): Client " << GetPeerNameString(clientSocket)
                  << " (socket " << clientSocket << ") closed connection normally.");
    CleanupClient(clientSocket);
}

void LoadBalancerApp::HandleClientError(Ptr<Socket> clientSocket)
{
    NS_LOG_FUNCTION(this << clientSocket);
    Socket::SocketErrno err = clientSocket->GetErrno();
    NS_LOG_WARN("LB (L7): Error on client socket " << clientSocket << " (" << GetPeerNameString(clientSocket)
                  << "). Errno: " << err << " (" << std::strerror(err) << ")");
    CleanupClient(clientSocket);
}

void LoadBalancerApp::HandleBackendClose(Ptr<Socket> backendSocket)
{
    NS_LOG_FUNCTION(this << backendSocket);
    InetSocketAddress backendAddress(Ipv4Address::GetAny(),0);
    bool addrKnown = false;
    Address peerAddr;
    if(backendSocket && backendSocket->GetPeerName(peerAddr) == 0 && InetSocketAddress::IsMatchingType(peerAddr)) {
        backendAddress = InetSocketAddress::ConvertFrom(peerAddr);
        addrKnown = true;
    }
    NS_LOG_INFO("LB (L7): Backend " << (addrKnown ? InetAddressToString(backendAddress) : GetPeerNameString(backendSocket))
                  << " (socket " << backendSocket << ") closed connection normally.");

    if (addrKnown) {
        uint32_t count = 0;
        for(auto it = m_requestSendTimes.begin(); it != m_requestSendTimes.end(); ) {
            if(it->first.first == backendSocket) { 
                NotifyRequestFinished(backendAddress); 
                count++;
                it = m_requestSendTimes.erase(it);
            } else {
                ++it;
            }
        }
        if(count > 0) NS_LOG_DEBUG(" -- Notified request finished for " << count
                                   << " outstanding requests on normally closed backend " << backendAddress);
    } else {
        NS_LOG_WARN(" -- Could not get backend address for normally closed socket " << backendSocket
                      << " to notify finish for outstanding requests.");
    }
    CleanupBackendSocket(backendSocket);
}

void LoadBalancerApp::HandleBackendError(Ptr<Socket> backendSocket)
{
    NS_LOG_FUNCTION(this << backendSocket);
    Socket::SocketErrno err = backendSocket->GetErrno();
    InetSocketAddress backendAddress(Ipv4Address::GetAny(),0);
    bool addrKnown = false;
    Address peerAddr;

    if(backendSocket && backendSocket->GetPeerName(peerAddr) == 0 && InetSocketAddress::IsMatchingType(peerAddr)) {
        backendAddress = InetSocketAddress::ConvertFrom(peerAddr);
        addrKnown = true;
    }
    NS_LOG_WARN("LB (L7): Error on backend socket " << backendSocket << " ("
                  << (addrKnown ? InetAddressToString(backendAddress) : GetPeerNameString(backendSocket))
                  << "). Errno: " << err << " (" << std::strerror(err) << ")");

    auto pending_it = m_pendingBackendRequests.find(backendSocket);
    if (pending_it != m_pendingBackendRequests.end()) {
        InetSocketAddress targetAddr = pending_it->second.targetBackendAddress;
        NS_LOG_WARN(" -- Backend error occurred on a socket with a PENDING connection request to " << targetAddr);
        NotifyRequestFinished(targetAddr); 
    } else if (addrKnown) {
        uint32_t count = 0;
        for(auto it = m_requestSendTimes.begin(); it != m_requestSendTimes.end(); ) {
            if(it->first.first == backendSocket) {
                NotifyRequestFinished(backendAddress);
                count++;
                it = m_requestSendTimes.erase(it);
            } else {
                ++it;
            }
        }
        if(count > 0) NS_LOG_DEBUG(" -- Notified request finished for " << count
                                   << " outstanding requests on errored backend " << backendAddress);
    } else {
        NS_LOG_WARN(" -- Could not determine address for errored backend socket " << backendSocket
                      << " to precisely notify request finished for outstanding requests.");
    }
    CleanupBackendSocket(backendSocket);
}


void LoadBalancerApp::CleanupClient(Ptr<Socket> clientSocket) {
    NS_LOG_FUNCTION(this << clientSocket);
    if (!clientSocket) {
        NS_LOG_DEBUG("CleanupClient called with null socket.");
        return;
    }
    NS_LOG_INFO("LB (L7): Cleaning up client socket " << clientSocket << " (" << GetPeerNameString(clientSocket) << ")");

    auto client_backends_it = m_clientBackendSockets.find(clientSocket);
    if (client_backends_it != m_clientBackendSockets.end()) {
        NS_LOG_DEBUG(" -- Found " << client_backends_it->second.size() << " associated backend sockets for client " << clientSocket);
        std::vector<Ptr<Socket>> backendSocketsToCleanup; 
        for (auto const& [addr, backendSock] : client_backends_it->second) {
            if(backendSock) backendSocketsToCleanup.push_back(backendSock);
        }
        for (Ptr<Socket> backendSock : backendSocketsToCleanup) {
            CleanupBackendSocket(backendSock);
        }
        m_clientBackendSockets.erase(client_backends_it);
    } else {
        NS_LOG_DEBUG(" -- No backend socket map found for client " << clientSocket << " in m_clientBackendSockets.");
    }

    m_clientRxBuffers.erase(clientSocket);

    for (auto it = m_pendingBackendRequests.begin(); it != m_pendingBackendRequests.end(); ) {
        if (it->second.clientSocket == clientSocket) {
            Ptr<Socket> pendingBackendSock = it->first;
            InetSocketAddress targetAddr = it->second.targetBackendAddress;
            RequestResponseHeader reqHeader;
            it->second.requestPacket->PeekHeader(reqHeader);
            NS_LOG_WARN(" -- Cleaning up PENDING request (Seq=" << reqHeader.GetSeq() << ") to " << targetAddr
                          << " (backend socket " << pendingBackendSock << ") due to originating client " << clientSocket << " closing.");

            NotifyRequestFinished(targetAddr); 

            it = m_pendingBackendRequests.erase(it); 
            CleanupBackendSocket(pendingBackendSock); 
        } else {
            ++it;
        }
    }

    if (clientSocket->GetErrno() != Socket::ERROR_SHUTDOWN) { 
        NS_LOG_DEBUG(" -- Nullifying callbacks and closing client socket " << clientSocket);
        clientSocket->SetRecvCallback(MakeNullCallback<void, Ptr<Socket>>());
        clientSocket->SetSendCallback(MakeNullCallback<void, Ptr<Socket>, uint32_t>());
        clientSocket->SetCloseCallbacks(MakeNullCallback<void, Ptr<Socket>>(), MakeNullCallback<void, Ptr<Socket>>());
        clientSocket->Close();
    } else {
         NS_LOG_DEBUG(" -- Client socket " << clientSocket << " already shut down.");
    }
    NS_LOG_DEBUG("LB (L7): Client socket " << clientSocket << " cleanup finished.");
}


void LoadBalancerApp::CleanupBackendSocket(Ptr<Socket> backendSocket, bool mapEraseOnly) {
    NS_LOG_FUNCTION(this << backendSocket << mapEraseOnly);
    if (!backendSocket) {
        NS_LOG_DEBUG("CleanupBackendSocket called with null socket.");
        return;
    }

    InetSocketAddress backendAddressForNotify(Ipv4Address::GetAny(),0); 
    bool addrForNotifyKnown = false;
    std::string backendSocketIdStr; // For logging

    Address rawPeerAddr;
    if(backendSocket->GetPeerName(rawPeerAddr) == 0 && InetSocketAddress::IsMatchingType(rawPeerAddr)) {
        backendAddressForNotify = InetSocketAddress::ConvertFrom(rawPeerAddr);
        addrForNotifyKnown = true;
        backendSocketIdStr = InetAddressToString(backendAddressForNotify);
    } else {
        backendSocketIdStr = GetPeerNameString(backendSocket); // Fallback if GetPeerName failed or wrong type
    }


    NS_LOG_INFO("LB (L7): Cleaning up backend socket " << backendSocket << " (" << backendSocketIdStr << ")"
                  << (mapEraseOnly ? " (map erase only)" : ""));

    Ptr<Socket> clientSocket = nullptr;
    auto backend_client_it = m_backendClientMap.find(backendSocket);
    if (backend_client_it != m_backendClientMap.end()) {
        clientSocket = backend_client_it->second;
        m_backendClientMap.erase(backend_client_it);
    }

    if (clientSocket) {
        auto client_backends_it = m_clientBackendSockets.find(clientSocket);
        if (client_backends_it != m_clientBackendSockets.end()) {
            for (auto it = client_backends_it->second.begin(); it != client_backends_it->second.end(); ++it) {
                if (it->second == backendSocket) {
                    if (!addrForNotifyKnown) { 
                        backendAddressForNotify = it->first;
                        addrForNotifyKnown = true;
                    }
                    client_backends_it->second.erase(it);
                    break;
                }
            }
        }
    }

    m_backendRxBuffers.erase(backendSocket);

    auto pending_it = m_pendingBackendRequests.find(backendSocket);
    if (pending_it != m_pendingBackendRequests.end()) {
        if (!addrForNotifyKnown) { 
            backendAddressForNotify = pending_it->second.targetBackendAddress;
            addrForNotifyKnown = true;
        }
        NS_LOG_DEBUG(" -- Removing entry from pending requests for socket " << backendSocket);
        m_pendingBackendRequests.erase(pending_it);
    }

    uint32_t removed_send_times = 0;
    for (auto it = m_requestSendTimes.begin(); it != m_requestSendTimes.end(); ) {
        if (it->first.first == backendSocket) {
            if (addrForNotifyKnown) { 
                NotifyRequestFinished(backendAddressForNotify);
            } else {
                 NS_LOG_WARN(" -- Cannot notify request finished for outstanding request on socket "
                               << backendSocket << ", backend address unknown.");
            }
            it = m_requestSendTimes.erase(it);
            removed_send_times++;
        } else {
            ++it;
        }
    }
    if(removed_send_times > 0) NS_LOG_DEBUG(" -- Removed and notified finish for " << removed_send_times
                                           << " entries from m_requestSendTimes for backend socket " << backendSocket);

    if (!mapEraseOnly) {
        if (backendSocket->GetErrno() != Socket::ERROR_SHUTDOWN) { 
            NS_LOG_DEBUG(" -- Nullifying callbacks and closing backend socket " << backendSocket);
            backendSocket->SetRecvCallback(MakeNullCallback<void, Ptr<Socket>>());
            backendSocket->SetSendCallback(MakeNullCallback<void, Ptr<Socket>, uint32_t>());
            backendSocket->SetCloseCallbacks(MakeNullCallback<void, Ptr<Socket>>(), MakeNullCallback<void, Ptr<Socket>>());
            backendSocket->SetConnectCallback(MakeNullCallback<void, Ptr<Socket>>(), MakeNullCallback<void, Ptr<Socket>>());
            backendSocket->Close();
        } else {
            NS_LOG_DEBUG(" -- Backend socket " << backendSocket << " already shut down.");
        }
    }
    NS_LOG_DEBUG("LB (L7): Backend socket " << backendSocket << " cleanup finished.");
}

} // namespace ns3
