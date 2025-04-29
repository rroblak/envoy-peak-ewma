#include "latency_client_app.h"

#include "request_response_header.h" // Custom header
#include "ns3/log.h"
#include "ns3/ipv4-address.h"
#include "ns3/nstime.h" // For Time, Seconds, Simulator::Now()
#include "ns3/inet-socket-address.h"
#include "ns3/socket.h"
#include "ns3/simulator.h" // For Simulator schedule/cancel
#include "ns3/socket-factory.h"
#include "ns3/packet.h"
#include "ns3/uinteger.h"
#include "ns3/tcp-socket-factory.h"
#include "ns3/core-module.h"    // For Ptr, ObjectFactory, TypeId, Callbacks, App basics
#include "ns3/buffer.h"

#include <string>
#include <vector>
#include <map>
#include <random>
#include <limits>
#include <cstdint> // Included via latency_client_app.h but good practice here too

namespace ns3 {

NS_LOG_COMPONENT_DEFINE("LatencyClientApp");
// NS_OBJECT_ENSURE_REGISTERED is typically in the .h for classes using GetTypeId,
// but if it was here before for a specific reason, it could remain.
// However, standard practice is in .h or not at all if GetTypeId is sufficient.
// For now, assuming it's handled by the .h based on common practice.

TypeId
LatencyClientApp::GetTypeId()
{
    static TypeId tid =
        TypeId("ns3::LatencyClientApp")
            .SetParent<Application>()
            .SetGroupName("Applications")
            .AddConstructor<LatencyClientApp>()
            .AddAttribute("RemoteIpAddress",
                          "The destination IPv4 Address of the outbound packets.",
                          Ipv4AddressValue("127.0.0.1"),
                          MakeIpv4AddressAccessor(&LatencyClientApp::m_peerIpv4Address),
                          MakeIpv4AddressChecker())
            .AddAttribute("RemotePort",
                          "The destination port of the outbound packets.",
                          UintegerValue(0),
                          MakeUintegerAccessor(&LatencyClientApp::m_peerPort),
                          MakeUintegerChecker<uint16_t>())
            .AddAttribute("RequestCount",
                          "Number of requests to send (0 for continuous until stop time).",
                          UintegerValue(100),
                          MakeUintegerAccessor(&LatencyClientApp::m_requestCount),
                          MakeUintegerChecker<uint32_t>())
            .AddAttribute("RequestInterval",
                          "Time between sending requests.",
                          TimeValue(Seconds(1.0)),
                          MakeTimeAccessor(&LatencyClientApp::m_requestInterval),
                          MakeTimeChecker())
            .AddAttribute("RequestSize",
                          "Size of the request payload (bytes).",
                          UintegerValue(100),
                          MakeUintegerAccessor(&LatencyClientApp::m_requestSize),
                          MakeUintegerChecker<uint32_t>());
    return tid;
}

LatencyClientApp::LatencyClientApp()
    : m_socket(nullptr),
      m_peerPort(0), // Will be set by attribute or SetRemote
      m_requestSize(0), // Will be set by attribute
      m_requestCount(0), // Will be set by attribute
      m_requestInterval(Seconds(0)), // Will be set by attribute
      m_seqCounter(0),
      m_requestsSent(0),
      m_responsesReceived(0),
      m_running(false),
      m_connected(false),
      m_rng(std::random_device{}() + static_cast<uint64_t>(Simulator::GetContext())),
      m_dist(0, std::numeric_limits<uint64_t>::max())
{
    NS_LOG_FUNCTION(this);
    // m_peerIpv4Address is default constructed by Ipv4Address()
}

LatencyClientApp::~LatencyClientApp()
{
    NS_LOG_FUNCTION(this);
    m_socket = nullptr;
}

void
LatencyClientApp::SetRemote(Ipv4Address ip, uint16_t port)
{
    NS_LOG_FUNCTION(this << ip << port);
    m_peerIpv4Address = ip;
    m_peerPort = port;
}

void
LatencyClientApp::SetRemote(InetSocketAddress address)
{
    NS_LOG_FUNCTION(this << address);
    m_peerIpv4Address = address.GetIpv4();
    m_peerPort = address.GetPort();
}

void
LatencyClientApp::SetRequestCount(uint32_t count)
{
    NS_LOG_FUNCTION(this << count);
    m_requestCount = count;
}

void
LatencyClientApp::SetRequestInterval(Time interval)
{
    NS_LOG_FUNCTION(this << interval);
    m_requestInterval = interval;
}

void
LatencyClientApp::SetRequestSize(uint32_t size)
{
    NS_LOG_FUNCTION(this << size);
    m_requestSize = size;
}

const std::vector<Time>&
LatencyClientApp::GetLatencies() const
{
    return m_latencies;
}

void
LatencyClientApp::DoDispose()
{
    NS_LOG_FUNCTION(this);
    if (m_socket) {
        m_socket->Close();
        m_socket = nullptr;
    }
    m_connected = false;
    m_running = false;
    Simulator::Cancel(m_sendEvent);
    Application::DoDispose();
}

void
LatencyClientApp::StartApplication()
{
    NS_LOG_FUNCTION(this);
    NS_LOG_INFO(Simulator::Now().GetSeconds() << "s LatencyClientApp on Node " << GetNode()->GetId() << " starting.");

    m_running = true;
    m_requestsSent = 0;
    m_responsesReceived = 0;
    m_seqCounter = 0;
    m_latencies.clear();
    m_sentTimes.clear();
    m_rxBuffer.clear();

    if (m_peerIpv4Address == Ipv4Address() || m_peerIpv4Address == Ipv4Address::GetAny() || m_peerPort == 0) {
        NS_LOG_ERROR("Client (Node " << GetNode()->GetId() << ") has invalid remote IP/port. Stopping. Addr: "
                      << m_peerIpv4Address << " Port: " << m_peerPort);
        m_running = false;
        return;
    }

    if (!m_socket)
    {
        SetupSocket();
    }

    Connect();
}

void
LatencyClientApp::StopApplication()
{
    NS_LOG_FUNCTION(this);
    NS_LOG_INFO(Simulator::Now().GetSeconds() << "s LatencyClientApp on Node " << GetNode()->GetId() << " stopping.");

    m_running = false;

    if (m_sendEvent.IsPending())
    {
        NS_LOG_DEBUG("Cancelling pending send event during StopApplication.");
        Simulator::Cancel(m_sendEvent);
    }

    if (m_socket) // Check if socket exists before trying to close
    {
        NS_LOG_DEBUG("Closing client socket during StopApplication (if connected or exists).");
        m_socket->Close();
        // m_connected will be set to false by HandleClose or HandleError callbacks
    }


    NS_LOG_INFO("Client (Node " << GetNode()->GetId() << ") Summary: Requests Sent=" << m_requestsSent
                  << ", Responses Received=" << m_responsesReceived
                  << ", Latencies Recorded=" << m_latencies.size());
}

void
LatencyClientApp::SetupSocket()
{
    NS_LOG_FUNCTION(this);
    if (m_socket == nullptr) {
        m_socket = Socket::CreateSocket(GetNode(), TcpSocketFactory::GetTypeId());
        if (!m_socket) {
            NS_FATAL_ERROR("Failed to create client socket on Node " << GetNode()->GetId());
            // NS_FATAL_ERROR will terminate, so return is not strictly needed but good form.
            return;
        }

        m_socket->SetConnectCallback(MakeCallback(&LatencyClientApp::ConnectionSucceeded, this),
                                     MakeCallback(&LatencyClientApp::ConnectionFailed, this));
        m_socket->SetCloseCallbacks(MakeCallback(&LatencyClientApp::HandleClose, this),
                                    MakeCallback(&LatencyClientApp::HandleError, this));
        m_socket->SetRecvCallback(MakeCallback(&LatencyClientApp::HandleRead, this));
        m_socket->SetSendCallback(MakeCallback(&LatencyClientApp::HandleSend, this));
    }
}


void LatencyClientApp::Connect() {
    NS_LOG_FUNCTION(this);
    if (!m_socket) {
        NS_LOG_ERROR("Connect called on Node " << GetNode()->GetId() << " but socket is null. Attempting setup.");
        SetupSocket(); // Try to set it up if null
        if(!m_socket) { // If still null after setup attempt
             NS_LOG_ERROR("Socket setup failed in Connect() for Node " << GetNode()->GetId() << ". Cannot connect.");
             return;
        }
    }
    if (m_connected) { // Or if a connection attempt is already in progress
        NS_LOG_WARN("Connect called on Node " << GetNode()->GetId() << " but already connected or connecting.");
        return;
    }

    InetSocketAddress remoteAddress(m_peerIpv4Address, m_peerPort);
    NS_LOG_INFO("Client (Node " << GetNode()->GetId() << ") attempting to connect to " << remoteAddress);
    m_socket->Connect(remoteAddress);
}

void
LatencyClientApp::ConnectionSucceeded(Ptr<Socket> socket)
{
    NS_LOG_FUNCTION(this << socket);
    InetSocketAddress remoteAddress(m_peerIpv4Address, m_peerPort);
    NS_LOG_INFO(Simulator::Now().GetSeconds() << "s Client (Node " << GetNode()->GetId()
                  << ") connection SUCCEEDED to " << remoteAddress);
    m_connected = true;

    if (m_running) {
        Simulator::ScheduleNow(&LatencyClientApp::SendRequestPacket, this);
    }
}

void
LatencyClientApp::ConnectionFailed(Ptr<Socket> socket)
{
    NS_LOG_FUNCTION(this << socket);
    InetSocketAddress remoteAddress(m_peerIpv4Address, m_peerPort);
    NS_LOG_ERROR(Simulator::Now().GetSeconds() << "s Client (Node " << GetNode()->GetId()
                   << ") connection FAILED to " << remoteAddress << ". Errno: " << socket->GetErrno()); // Corrected
    m_connected = false;
}

void
LatencyClientApp::HandleClose(Ptr<Socket> socket)
{
    NS_LOG_FUNCTION(this << socket);
    NS_LOG_INFO(Simulator::Now().GetSeconds() << "s Client (Node " << GetNode()->GetId() << ") socket closed (normal).");
    m_connected = false;
    if (m_sendEvent.IsPending())
    {
        Simulator::Cancel(m_sendEvent);
    }
}

void
LatencyClientApp::HandleError(Ptr<Socket> socket)
{
    NS_LOG_FUNCTION(this << socket);
    NS_LOG_WARN(Simulator::Now().GetSeconds() << "s Client (Node " << GetNode()->GetId()
                  << ") socket error. Errno: " << socket->GetErrno()); // Corrected
    m_connected = false;
    if (m_sendEvent.IsPending())
    {
        Simulator::Cancel(m_sendEvent);
    }
    if (m_socket && m_socket->GetErrno() != Socket::ERROR_SHUTDOWN && m_socket->GetErrno() != Socket::ERROR_NOTCONN) {
        // Avoid re-closing if already shut down or not connected,
        // as Close() might have its own side effects or state checks.
        // Check GetErrno specifically to decide if Close is appropriate.
        // If error implies connection is terminally gone, Close() is mostly for resource cleanup.
        m_socket->Close();
    }
}

void
LatencyClientApp::HandleSend(Ptr<Socket> socket, uint32_t availableBytes)
{
    NS_LOG_FUNCTION(this << socket << availableBytes);
    NS_LOG_DEBUG("Client (Node " << GetNode()->GetId() << ") HandleSend: "
                   << availableBytes << " bytes available in send buffer.");
}

void
LatencyClientApp::HandleRead(Ptr<Socket> socket)
{
    NS_LOG_FUNCTION(this << socket);
    Ptr<Packet> packet;
    Address from;
    const uint32_t headerSize = RequestResponseHeader().GetSerializedSize();

    while ((packet = socket->RecvFrom(from)))
    {
        if (packet->GetSize() == 0)
        {
            NS_LOG_INFO(Simulator::Now().GetSeconds() << "s Client (Node " << GetNode()->GetId()
                          << "): Peer closed connection.");
            break;
        }

        std::string received_chunk(packet->GetSize(), '\0');
        packet->CopyData(reinterpret_cast<uint8_t*>(received_chunk.data()), packet->GetSize());
        m_rxBuffer.append(received_chunk);

        NS_LOG_DEBUG("Client (Node " << GetNode()->GetId() << ") HandleRead: Received "
                       << packet->GetSize() << " bytes. Buffer size: " << m_rxBuffer.size());

        while (m_rxBuffer.size() >= headerSize)
        {
            Ptr<Packet> headerPeekPacket = Create<Packet>(
                reinterpret_cast<const uint8_t*>(m_rxBuffer.data()),
                headerSize);

            RequestResponseHeader respHeader;
            if (headerPeekPacket->PeekHeader(respHeader) != headerSize) {
                 NS_LOG_WARN("Client (Node " << GetNode()->GetId()
                               << "): Could not peek complete header from buffer.");
                 break;
            }

            uint32_t expectedResponsePayloadSize = respHeader.GetPayloadSize();
            uint32_t expectedTotalSize = headerSize + expectedResponsePayloadSize;

            if (m_rxBuffer.size() >= expectedTotalSize)
            {
                NS_LOG_DEBUG("Client (Node " << GetNode()->GetId() << ") HandleRead: Processing complete response. Seq="
                               << respHeader.GetSeq() << ", Expected total size=" << expectedTotalSize);

                auto it = m_sentTimes.find(respHeader.GetSeq());
                if (it != m_sentTimes.end())
                {
                    Time sendTime = it->second;
                    Time latency = Simulator::Now() - sendTime;
                    m_latencies.push_back(latency);
                    m_sentTimes.erase(it);
                    m_responsesReceived++;
                    NS_LOG_INFO(Simulator::Now().GetSeconds() << "s Client (Node " << GetNode()->GetId()
                                  << "): Received response Seq=" << respHeader.GetSeq()
                                  << ", Latency=" << latency.GetMilliSeconds() << "ms");
                }
                else
                {
                    NS_LOG_WARN("Client (Node " << GetNode()->GetId()
                                  << "): Received response for unknown/duplicate/timed-out Seq=" << respHeader.GetSeq());
                }

                m_rxBuffer.erase(0, expectedTotalSize);
                NS_LOG_DEBUG("Client (Node " << GetNode()->GetId() << ") HandleRead: Consumed "
                               << expectedTotalSize << " bytes. Buffer remaining: " << m_rxBuffer.size());
            }
            else
            {
                NS_LOG_DEBUG("Client (Node " << GetNode()->GetId()
                               << ") HandleRead: Incomplete response in buffer. Need " << expectedTotalSize
                               << ", have " << m_rxBuffer.size() << ". Waiting for more data.");
                break;
            }
        }
    }

    int sockErrno = socket->GetErrno();
    if (sockErrno != Socket::ERROR_NOTERROR && sockErrno != Socket::ERROR_AGAIN &&
        sockErrno != Socket::ERROR_SHUTDOWN && sockErrno != Socket::ERROR_NOTCONN)
    {
        NS_LOG_WARN("Client (Node " << GetNode()->GetId()
                      << ") HandleRead: Error after RecvFrom loop. Errno: " << socket->GetErrno()); // Corrected
    }
}


void
LatencyClientApp::ScheduleNextRequest()
{
    NS_LOG_FUNCTION(this);
    if (!m_running) {
        NS_LOG_DEBUG("Client (Node " << GetNode()->GetId() << "): Not scheduling next request, m_running is false.");
        return;
    }
    if (!m_connected) {
        NS_LOG_DEBUG("Client (Node " << GetNode()->GetId() << "): Not scheduling next request, not connected.");
        return;
    }

    if (m_requestCount == 0 || m_requestsSent < m_requestCount)
    {
        NS_LOG_DEBUG("Client (Node " << GetNode()->GetId() << "): Scheduling next request send in "
                       << m_requestInterval.GetSeconds() << "s");
        m_sendEvent = Simulator::Schedule(m_requestInterval, &LatencyClientApp::SendRequestPacket, this);
    }
    else if (m_requestCount > 0 && m_requestsSent >= m_requestCount)
    {
        Time closeDelay = Seconds(0.5);
        NS_LOG_INFO("Client (Node " << GetNode()->GetId() << "): All " << m_requestsSent
                      << " requests sent. Scheduling socket close in " << closeDelay.GetSeconds() << "s.");
        if (m_socket && m_connected) {
             Simulator::Schedule(closeDelay, &Socket::Close, m_socket);
        }
    }
}

void
LatencyClientApp::SendRequestPacket()
{
    NS_LOG_FUNCTION(this);

    if (!m_running) {
        NS_LOG_DEBUG("Client (Node " << GetNode()->GetId() << "): SendRequestPacket called but app not running.");
        return;
    }
    if (!m_connected) {
        NS_LOG_WARN("Client (Node " << GetNode()->GetId() << "): SendRequestPacket called but not connected.");
        return;
    }
    NS_ASSERT_MSG(m_socket != nullptr, "SendRequestPacket called with null socket");

    if (m_requestCount > 0 && m_requestsSent >= m_requestCount) {
        NS_LOG_DEBUG("Client (Node " << GetNode()->GetId() << "): Request count reached ("
                       << m_requestsSent << "/" << m_requestCount << "). Not sending more.");
        return;
    }

    m_requestsSent++;
    m_seqCounter++;

    RequestResponseHeader reqHeader;
    reqHeader.SetSeq(m_seqCounter);
    reqHeader.SetTimestamp(Simulator::Now());
    reqHeader.SetPayloadSize(m_requestSize);
    reqHeader.SetL7Identifier(m_dist(m_rng));

    Ptr<Packet> packet = Create<Packet>(m_requestSize);
    packet->AddHeader(reqHeader);

    m_sentTimes[m_seqCounter] = reqHeader.GetTimestamp();

    InetSocketAddress remoteAddress(m_peerIpv4Address, m_peerPort);
    NS_LOG_INFO(reqHeader.GetTimestamp().GetSeconds() << "s Client (Node " << GetNode()->GetId()
                  << "): Sending Req Seq=" << reqHeader.GetSeq()
                  << ", Size=" << packet->GetSize()
                  << ", L7Id=" << reqHeader.GetL7Identifier()
                  << " to " << remoteAddress);

    int bytesActuallySent = m_socket->Send(packet);

    if (bytesActuallySent < 0) {
        NS_LOG_ERROR("Client (Node " << GetNode()->GetId() << "): Error sending packet Seq="
                       << reqHeader.GetSeq() << ". Errno: " << m_socket->GetErrno()); // Corrected
    } else {
        if (static_cast<uint32_t>(bytesActuallySent) < packet->GetSize()) {
            NS_LOG_WARN("Client (Node " << GetNode()->GetId() << "): Could not send full packet Seq="
                          << reqHeader.GetSeq() << " immediately. Sent " << bytesActuallySent
                          << "/" << packet->GetSize() << ". TCP will manage." );
        }
        ScheduleNextRequest();
    }
}

} // namespace ns3