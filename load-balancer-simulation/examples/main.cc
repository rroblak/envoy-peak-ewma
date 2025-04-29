#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/internet-module.h"
#include "ns3/csma-module.h"
#include "ns3/applications-module.h"
#include "ns3/log.h"
#include "ns3/stats-module.h"

// Custom modules
#include "ns3/utils.h"
#include "ns3/topology.h"
#include "ns3/load_balancer.h"
#include "ns3/round_robin_load_balancer.h"
#include "ns3/least_request_load_balancer.h"
#include "ns3/random_load_balancer.h"
#include "ns3/ring_hash_load_balancer.h"
#include "ns3/maglev_load_balancer.h"
#include "ns3/peak_ewma_load_balancer.h"
#include "ns3/latency_client_app.h"
#include "ns3/latency_server_app.h"
#include "ns3/request_response_header.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <memory>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace ns3 {

NS_LOG_COMPONENT_DEFINE("LoadBalancerSimulationMain");

namespace { // Anonymous namespace for internal linkage helpers

constexpr uint32_t kDefaultWeight = 1;
constexpr double kDefaultDelayMs = 0.0;
constexpr double kDefaultClientStartTimeStaggerS = 0.001; // Stagger to avoid all clients starting simultaneously

// Helper to trim whitespace from both ends of a string segment.
// Modifies the input string.
void TrimWhitespace(std::string& s)
{
    s.erase(0, s.find_first_not_of(" \t\n\r\f\v"));
    s.erase(s.find_last_not_of(" \t\n\r\f\v") + 1);
}

template <typename T>
std::string FormatVectorContents(const std::vector<T>& vec)
{
    if (vec.empty())
    {
        return "[]";
    }
    std::stringstream ss;
    ss << "[";
    for (size_t i = 0; i < vec.size(); ++i)
    {
        ss << vec[i] << (i == vec.size() - 1 ? "" : ", ");
    }
    ss << "]";
    return ss.str();
}

// Helper to format time values in milliseconds for logging
std::string FormatTimeMs(ns3::Time t, int precision = 4)
{
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(precision) << t.GetMilliSeconds();
    return oss.str();
}

// Helper to format double values for logging
std::string FormatDouble(double val, int precision = 4)
{
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(precision) << val;
    return oss.str();
}


} // namespace

std::vector<uint32_t> ParseWeights(const std::string& weightsStr)
{
    std::vector<uint32_t> weights;
    std::stringstream ss(weightsStr);
    std::string segment;

    while (std::getline(ss, segment, ','))
    {
        TrimWhitespace(segment);
        if (segment.empty())
        {
            NS_LOG_WARN("Empty weight segment. Using default weight: " << kDefaultWeight);
            weights.push_back(kDefaultWeight);
            continue;
        }

        uint32_t weight;
        auto [ptr, ec] = std::from_chars(segment.data(), segment.data() + segment.size(), weight);
        if (ec == std::errc() && ptr == segment.data() + segment.size() && weight > 0)
        {
            weights.push_back(weight);
        }
        else
        {
            NS_LOG_WARN("Invalid weight segment: '" << segment << "'. Using default weight: " << kDefaultWeight);
            weights.push_back(kDefaultWeight);
        }
    }
    return weights;
}

std::vector<double> ParseDelays(const std::string& delaysStr)
{
    std::vector<double> delays;
    std::stringstream ss(delaysStr);
    std::string segment;

    while (std::getline(ss, segment, ','))
    {
        TrimWhitespace(segment);
        if (segment.empty())
        {
            NS_LOG_WARN("Empty delay segment. Using default delay: " << kDefaultDelayMs << "ms");
            delays.push_back(kDefaultDelayMs);
            continue;
        }

        double delay;
        auto [ptr, ec] = std::from_chars(segment.data(), segment.data() + segment.size(), delay);
        if (ec == std::errc() && ptr == segment.data() + segment.size() && delay >= 0.0)
        {
            delays.push_back(delay);
        }
        else
        {
            NS_LOG_WARN("Invalid delay segment: '" << segment << "'. Using default delay: " << kDefaultDelayMs << "ms");
            delays.push_back(kDefaultDelayMs);
        }
    }
    return delays;
}

Time CalculatePercentile(std::vector<Time>& sortedData, double percentile)
{
    if (sortedData.empty() || percentile < 0.0 || percentile > 1.0)
    {
        NS_LOG_WARN("Invalid input for CalculatePercentile: empty data or percentile out of [0,1] range. Percentile: " << percentile);
        return Seconds(0.0);
    }

    const double n = static_cast<double>(sortedData.size());
    if (n == 1.0) { 
        return sortedData[0];
    }

    const double h = (n - 1.0) * percentile;
    const size_t lowerIdx = static_cast<size_t>(std::floor(h));
    size_t upperIdx = static_cast<size_t>(std::ceil(h));

    if (upperIdx >= sortedData.size()) {
        upperIdx = sortedData.size() - 1;
    }

    const Time lowerVal = sortedData[lowerIdx];
    const Time upperVal = sortedData[upperIdx];

    if (lowerIdx == upperIdx) 
    {
        return lowerVal;
    }

    const double fraction = h - static_cast<double>(lowerIdx);
    const int64_t lowerNs = lowerVal.GetNanoSeconds();
    const int64_t upperNs = upperVal.GetNanoSeconds();
    const int64_t interpolatedNs = static_cast<int64_t>(std::round(static_cast<double>(lowerNs) + fraction * (static_cast<double>(upperNs) - static_cast<double>(lowerNs))));

    return NanoSeconds(interpolatedNs);
}


int MainSimulation(int argc, char* argv[])
{
    // Simulation Parameters - Default values
    uint32_t numClients = 10;
    uint32_t numServers = 10;
    double simStopTimeS = 15.0;
    double clientAppStartTimeS = 1.0;
    double serverAppStartTimeS = 0.5;
    double lbAppStartTimeS = 0.5;
    std::string lbVipAddressStr = "192.168.1.1";
    std::string serverWeightsStr = "1,1,1,1,1,1,1,1,1,1";
    std::string lbAlgorithm = "PeakEWMA"; 
    uint32_t clientRequestCount = 100;
    double clientRequestIntervalS = 0.1;
    uint32_t clientRequestSizeBytes = 100;
    std::string serverDelaysStr = "5,5,5,5,5,5,5,5,5,50";

    // Command Line Argument Parsing
    CommandLine cmd(__FILE__);
    cmd.AddValue("numClients", "Number of client nodes", numClients);
    cmd.AddValue("numServers", "Number of backend server nodes", numServers);
    cmd.AddValue("simTime", "Total simulation time in seconds", simStopTimeS);
    cmd.AddValue("vip", "Load Balancer Virtual IP Address", lbVipAddressStr);
    cmd.AddValue("weights", "Comma-separated list of server weights (e.g., '2,1,1')", serverWeightsStr);
    cmd.AddValue("lbAlgorithm", "Load balancing algorithm (WRR, LR, Random, RingHash, Maglev, PeakEWMA)", lbAlgorithm);
    cmd.AddValue("reqCount", "Number of requests per client (0 for continuous)", clientRequestCount);
    cmd.AddValue("reqInterval", "Interval between client requests (seconds)", clientRequestIntervalS);
    cmd.AddValue("reqSize", "Payload size of client requests (bytes)", clientRequestSizeBytes);
    cmd.AddValue("serverDelays", "Comma-separated list of server processing delays (milliseconds, e.g., '0,10,10')", serverDelaysStr);
    cmd.Parse(argc, argv);

    if (numServers == 0 && lbAlgorithm != "None") { 
        NS_LOG_WARN("Number of servers is 0. Load balancer may not function as expected depending on algorithm.");
    }

    const Time clientRequestInterval = Seconds(clientRequestIntervalS);
    std::vector<uint32_t> serverWeights = ParseWeights(serverWeightsStr);
    std::vector<double> serverDelaysMs = ParseDelays(serverDelaysStr);

    // Adjust weights vector size to match numServers
    if (serverWeights.size() < numServers) {
        NS_LOG_WARN("Weights count (" << serverWeights.size() << ") < numServers (" << numServers
                      << "). Assigning default weight (" << kDefaultWeight << ") to remaining servers.");
        serverWeights.resize(numServers, kDefaultWeight);
    } else if (serverWeights.size() > numServers) {
        NS_LOG_WARN("Weights count (" << serverWeights.size() << ") > numServers (" << numServers
                      << "). Ignoring extra weights.");
        serverWeights.resize(numServers);
    }

    // Adjust delays vector size to match numServers
    if (serverDelaysMs.size() < numServers) {
        NS_LOG_WARN("Delays count (" << serverDelaysMs.size() << ") < numServers (" << numServers
                      << "). Assigning default delay (" << kDefaultDelayMs << "ms) to remaining servers.");
        serverDelaysMs.resize(numServers, kDefaultDelayMs);
    } else if (serverDelaysMs.size() > numServers) {
        NS_LOG_WARN("Delays count (" << serverDelaysMs.size() << ") > numServers (" << numServers
                      << "). Ignoring extra delays.");
        serverDelaysMs.resize(numServers);
    }

    // Logging Configuration
    LogComponentEnable("LoadBalancerSimulationMain", LOG_LEVEL_INFO);
    LogComponentEnable("SimulationUtils", LOG_LEVEL_WARN); 
    LogComponentEnable("TopologyCreator", LOG_LEVEL_WARN); 
    LogComponentEnable("LoadBalancerApp", LOG_LEVEL_INFO);
    LogComponentEnable("WeightedRoundRobinLoadBalancer", LOG_LEVEL_WARN);
    LogComponentEnable("LeastRequestLoadBalancer", LOG_LEVEL_INFO);
    LogComponentEnable("RandomLoadBalancer", LOG_LEVEL_WARN);
    LogComponentEnable("RingHashLoadBalancer", LOG_LEVEL_WARN);
    LogComponentEnable("MaglevLoadBalancer", LOG_LEVEL_WARN);
    LogComponentEnable("PeakEwmaLoadBalancer", LOG_LEVEL_INFO);
    LogComponentEnable("LatencyClientApp", LOG_LEVEL_INFO);
    LogComponentEnable("LatencyServerApp", LOG_LEVEL_WARN);
    LogComponentEnable("RequestResponseHeader", LOG_LEVEL_WARN);

    // Simulation Setup Information
    NS_LOG_INFO("--- NS-3 Load Balancer Simulation (Latency Measurement) ---");
    NS_LOG_INFO("Configuration: " << numClients << " Clients, " << numServers << " Servers, LB Algo: " << lbAlgorithm);
    NS_LOG_INFO("Server Weights: " << FormatVectorContents(serverWeights));
    NS_LOG_INFO("Server Delays (ms): " << FormatVectorContents(serverDelaysMs));
    NS_LOG_INFO("Client Config: " << (clientRequestCount == 0 ? "Continuous" : std::to_string(clientRequestCount)) << " req/client, "
                  << clientRequestInterval.GetSeconds() << "s interval, "
                  << clientRequestSizeBytes << " byte payload");
    NS_LOG_INFO("Load Balancer VIP: " << lbVipAddressStr << ":" << LB_PORT); 
    NS_LOG_INFO("Simulation Stop Time: " << simStopTimeS << "s");


    // Topology and Network Infrastructure Setup
    NodeContainer clientNodes;
    Ptr<Node> lbNode;
    NodeContainer serverNodes;
    InternetStackHelper internetStack;
    CreateTopology(numClients, numServers, clientNodes, lbNode, serverNodes, internetStack); 

    // Load Balancer Application Setup
    ObjectFactory lbFactory;
    if (lbAlgorithm == "WRR") {
        lbFactory.SetTypeId(WeightedRoundRobinLoadBalancer::GetTypeId());
    } else if (lbAlgorithm == "LR") {
        lbFactory.SetTypeId(LeastRequestLoadBalancer::GetTypeId());
    } else if (lbAlgorithm == "Random") {
        lbFactory.SetTypeId(RandomLoadBalancer::GetTypeId());
    } else if (lbAlgorithm == "RingHash") {
        lbFactory.SetTypeId(RingHashLoadBalancer::GetTypeId());
    } else if (lbAlgorithm == "Maglev") {
        lbFactory.SetTypeId(MaglevLoadBalancer::GetTypeId());
    } else if (lbAlgorithm == "PeakEWMA") {
        lbFactory.SetTypeId(PeakEwmaLoadBalancer::GetTypeId());
    } else {
        NS_FATAL_ERROR("Invalid load balancing algorithm: " << lbAlgorithm << ". Supported: WRR, LR, Random, RingHash, Maglev, PeakEWMA.");
    }
    lbFactory.Set("Port", UintegerValue(LB_PORT)); 

    Ptr<LoadBalancerApp> lbApp = lbFactory.Create<LoadBalancerApp>();
    NS_ASSERT_MSG(lbApp, "Failed to create LoadBalancerApp instance.");
    lbNode->AddApplication(lbApp);
    lbApp->SetStartTime(Seconds(lbAppStartTimeS));
    lbApp->SetStopTime(Seconds(simStopTimeS));

    // Backend Server Applications Setup
    NS_LOG_INFO("Setting up " << numServers << " Backend Servers (LatencyServerApp)...");
    ApplicationContainer serverApps;
    ObjectFactory serverFactory;
    serverFactory.SetTypeId(LatencyServerApp::GetTypeId());
    serverFactory.Set("Port", UintegerValue(SERVER_PORT)); 

    for (uint32_t i = 0; i < numServers; ++i)
    {
        Ptr<Node> serverNode = serverNodes.Get(i);
        Ptr<Application> app = serverFactory.Create<Application>();
        NS_ASSERT_MSG(app, "Failed to create server Application instance.");

        Ptr<LatencyServerApp> latencyApp = DynamicCast<LatencyServerApp>(app);
        NS_ASSERT_MSG(latencyApp, "Failed to cast Application to LatencyServerApp for server " << i);
        latencyApp->SetProcessingDelay(MilliSeconds(serverDelaysMs[i]));
        
        serverNode->AddApplication(latencyApp);
        latencyApp->SetStartTime(Seconds(serverAppStartTimeS));
        latencyApp->SetStopTime(Seconds(simStopTimeS));
        serverApps.Add(latencyApp);

        InetSocketAddress backendAddr(GetIpv4Address(serverNode, 1), SERVER_PORT); 
        lbApp->AddBackend(backendAddr, serverWeights[i]);

        NS_LOG_INFO("  Server " << i << " (Node " << serverNode->GetId()
                      << ", " << backendAddr.GetIpv4() << ":" << backendAddr.GetPort()
                      << ") installed. Weight: " << serverWeights[i] << ", Delay: " << serverDelaysMs[i] << "ms");
    }

    // Client Applications Setup
    NS_LOG_INFO("Setting up " << numClients << " Clients (LatencyClientApp)...");
    ApplicationContainer clientApps;
    ObjectFactory clientFactory;
    clientFactory.SetTypeId(LatencyClientApp::GetTypeId());
    clientFactory.Set("RemoteIpAddress", Ipv4AddressValue(lbVipAddressStr.c_str()));
    clientFactory.Set("RemotePort", UintegerValue(LB_PORT));
    clientFactory.Set("RequestCount", UintegerValue(clientRequestCount));
    clientFactory.Set("RequestInterval", TimeValue(clientRequestInterval));
    clientFactory.Set("RequestSize", UintegerValue(clientRequestSizeBytes));

    for (uint32_t i = 0; i < numClients; ++i)
    {
        Ptr<Node> clientNode = clientNodes.Get(i);
        Ptr<Application> app = clientFactory.Create<Application>();
        NS_ASSERT_MSG(app, "Failed to create client Application instance.");
        
        clientNode->AddApplication(app);
        app->SetStartTime(Seconds(clientAppStartTimeS + (static_cast<double>(i) * kDefaultClientStartTimeStaggerS)));
        app->SetStopTime(Seconds(simStopTimeS));
        clientApps.Add(app);

        NS_LOG_INFO("  Client " << i << " (Node " << clientNode->GetId()
                      << ") installed, targeting " << lbVipAddressStr << ":" << LB_PORT);
    }

    // Routing Configuration
    NS_LOG_INFO("Populating Global Routing Tables...");
    SetupRouting(); 

    // Simulation Execution
    NS_LOG_INFO("--- Running Simulation for " << simStopTimeS << " seconds ---");
    Simulator::Stop(Seconds(simStopTimeS + 1.0)); 
    Simulator::Run();
    NS_LOG_INFO("--- Simulation Finished ---");

    // Results Collection and Analysis: Latency
    std::vector<Time> allLatencies;
    uint64_t totalResponses = 0;
    for (uint32_t i = 0; i < clientApps.GetN(); ++i)
    {
        Ptr<LatencyClientApp> client = DynamicCast<LatencyClientApp>(clientApps.Get(i));
        if (client)
        {
            const auto& latencies = client->GetLatencies();
            allLatencies.insert(allLatencies.end(), latencies.begin(), latencies.end());
            totalResponses += latencies.size();
        }
    }
    
    uint64_t expectedTotalRequestsFromClients = (clientRequestCount > 0) ? (static_cast<uint64_t>(numClients) * clientRequestCount) : 0;


    NS_LOG_INFO("\n--- Latency Results (" << totalResponses << " responses recorded) ---");
    if (!allLatencies.empty())
    {
        std::sort(allLatencies.begin(), allLatencies.end());

        const Time minLatency = allLatencies.front();
        const Time maxLatency = allLatencies.back();
        const Time p50Latency = CalculatePercentile(allLatencies, 0.50);
        const Time p75Latency = CalculatePercentile(allLatencies, 0.75);
        const Time p90Latency = CalculatePercentile(allLatencies, 0.90);
        const Time p95Latency = CalculatePercentile(allLatencies, 0.95);
        const Time p99Latency = CalculatePercentile(allLatencies, 0.99);

        const double totalLatencyMsVal = std::accumulate(allLatencies.begin(), allLatencies.end(), 0.0,
            [](double sum, const Time& t){ return sum + t.GetMilliSeconds(); });
        
        const double avgLatencyMs = totalLatencyMsVal / static_cast<double>(allLatencies.size());

        const double varianceAccum = std::accumulate(allLatencies.begin(), allLatencies.end(), 0.0,
            [&](double acc, const Time& t){ return acc + std::pow(t.GetMilliSeconds() - avgLatencyMs, 2); });
        
        const double variance = varianceAccum / static_cast<double>(allLatencies.size());
        const double stdDevLatencyMs = std::sqrt(variance);

        NS_LOG_INFO("Min Latency:    " << FormatTimeMs(minLatency) << " ms");
        NS_LOG_INFO("Avg Latency:    " << FormatDouble(avgLatencyMs) << " ms");
        NS_LOG_INFO("P50 Latency:    " << FormatTimeMs(p50Latency) << " ms");
        NS_LOG_INFO("P75 Latency:    " << FormatTimeMs(p75Latency) << " ms");
        NS_LOG_INFO("P90 Latency:    " << FormatTimeMs(p90Latency) << " ms");
        NS_LOG_INFO("P95 Latency:    " << FormatTimeMs(p95Latency) << " ms");
        NS_LOG_INFO("P99 Latency:    " << FormatTimeMs(p99Latency) << " ms");
        NS_LOG_INFO("Max Latency:    " << FormatTimeMs(maxLatency) << " ms");
        NS_LOG_INFO("Std Dev:        " << FormatDouble(stdDevLatencyMs) << " ms");
    }
    else
    {
        NS_LOG_INFO("No latency data collected (0 responses received).");
    }
    NS_LOG_INFO("--------------------------------------------------");

    // Results Collection and Analysis: Server Request Distribution
    NS_LOG_INFO("\n--- Backend Server Request Distribution ---");
    uint64_t totalRequestsProcessedByServers = 0;
    for (uint32_t i = 0; i < serverApps.GetN(); ++i)
    {
        Ptr<LatencyServerApp> serverApp = DynamicCast<LatencyServerApp>(serverApps.Get(i));
        if (serverApp)
        {
            uint64_t count = serverApp->GetTotalRequestsReceived();
            Ptr<Node> serverNode = serverApp->GetNode();
            InetSocketAddress serverAddr(Ipv4Address::GetAny(),0); 
            if (serverNode && serverNode->GetNDevices() > 1) { 
                 try {
                    serverAddr = InetSocketAddress(GetIpv4Address(serverNode, 1), SERVER_PORT);
                 } catch (const std::runtime_error& e) {
                     NS_LOG_WARN("Could not get IP for server node " << serverNode->GetId()
                                 << " for logging counts: " << e.what());
                 }
            }
            NS_LOG_INFO("Server " << i << " (" << serverAddr.GetIpv4() << ":" << serverAddr.GetPort()
                      << ", W:" << serverWeights[i] << ", D:" << serverDelaysMs[i] << "ms): "
                      << count << " requests");
            totalRequestsProcessedByServers += count;
        }
        else
        {
             NS_LOG_WARN("Could not retrieve LatencyServerApp from application index " << i << " for stats.");
        }
    }
    NS_LOG_INFO("Total Requests Processed by Servers: " << totalRequestsProcessedByServers);
    
    if (expectedTotalRequestsFromClients > 0) { 
        if (totalRequestsProcessedByServers != expectedTotalRequestsFromClients) {
            NS_LOG_WARN("Mismatch: Client requests estimated sent (~" << expectedTotalRequestsFromClients
                        << ") vs. Server requests processed (" << totalRequestsProcessedByServers << "). "
                        << "Difference: " << std::abs(static_cast<long long>(totalRequestsProcessedByServers) - static_cast<long long>(expectedTotalRequestsFromClients))
                        << ". This can be due to packet drops, simulation ending before all responses, or client-side errors.");
        } else {
             NS_LOG_INFO("Server processed count matches estimated client sent count.");
        }
    } else if (clientRequestCount == 0) {
        NS_LOG_INFO("(Client request count was 0 - continuous sending; direct comparison not applicable.)");
    }
    NS_LOG_INFO("-----------------------------------------");

    // Cleanup
    Simulator::Destroy();
    NS_LOG_INFO("Simulator destroyed.");

    return 0;
}

} // namespace ns3

int main(int argc, char* argv[])
{
    ns3::Time::SetResolution(ns3::Time::NS);
    return ns3::MainSimulation(argc, argv);
}
