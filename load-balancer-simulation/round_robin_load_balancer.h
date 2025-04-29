#ifndef ROUND_ROBIN_LOAD_BALANCER_H
#define ROUND_ROBIN_LOAD_BALANCER_H

// Base class
#include "load_balancer.h"

// NS-3 Includes
#include "ns3/nstime.h" // For Time (used in overridden virtual function signatures)

// Standard Library Includes
#include <vector>
#include <utility> // For std::pair
#include <cstdint> // For uint32_t, int32_t, uint64_t
#include <cstddef> // For std::size_t

namespace ns3 {

// Forward declarations (if base class doesn't cover them for overridden methods)
class Packet;
class Address;
class InetSocketAddress;

/**
 * @brief Implements Weighted Round-Robin (WRR) load balancing.
 *
 * This load balancer distributes requests to backend servers in a round-robin fashion,
 * but gives more requests to servers with higher weights. It uses an algorithm
 * similar to the one described by Nginx, which involves tracking a current weight
 * and adjusting it based on the greatest common divisor (GCD) of all backend weights.
 *
 * The state for WRR (current index, current weight, max weight, GCD of weights)
 * is recalculated whenever the set of backend servers changes.
 */
class WeightedRoundRobinLoadBalancer : public LoadBalancerApp
{
  public:
    /**
     * @brief Gets the TypeId for this application.
     * @return The object TypeId.
     */
    static TypeId GetTypeId();

    WeightedRoundRobinLoadBalancer();
    virtual ~WeightedRoundRobinLoadBalancer() override;

    // Override backend management methods to trigger recalculation of WRR state
    virtual void SetBackends(const std::vector<std::pair<InetSocketAddress, uint32_t>>& backends) override;
    virtual void AddBackend(const InetSocketAddress& backendAddress, uint32_t weight) override;
    virtual void AddBackend(const InetSocketAddress& backendAddress) override; // Adds with default weight

  protected:
    /**
     * @brief Chooses the next backend server using the Weighted Round-Robin algorithm.
     *
     * @param packet The incoming request packet (content not used by this algorithm).
     * @param fromAddress The source address of the request (not used by this algorithm).
     * @param l7Identifier The L7 identifier from the request (not used by this algorithm).
     * @param[out] chosenBackend The InetSocketAddress of the selected backend server.
     * @return True if a backend was successfully chosen, false otherwise (e.g., no backends available or all have zero weight).
     */
    virtual bool ChooseBackend(Ptr<Packet> packet,
                               const Address& fromAddress,
                               uint64_t l7Identifier,
                               InetSocketAddress& chosenBackend) override;

    /**
     * @brief Records latency for a backend. No-op for WeightedRoundRobinLoadBalancer.
     * WRR selection is based on weights and round-robin sequence, not active latency.
     * @param backendAddress The address of the backend.
     * @param rtt The recorded round-trip time.
     */
    virtual void RecordBackendLatency(InetSocketAddress backendAddress, Time rtt) override;

    /**
     * @brief Notification that a request has been sent to a backend. No-op for WeightedRoundRobinLoadBalancer.
     * WRR does not use active request counts for selection.
     * @param backendAddress The address of the backend the request was sent to.
     */
    virtual void NotifyRequestSent(InetSocketAddress backendAddress) override;

    /**
     * @brief Notification that a request to a backend has finished. No-op for WeightedRoundRobinLoadBalancer.
     * WRR does not use active request counts for selection.
     * @param backendAddress The address of the backend whose request finished.
     */
    virtual void NotifyRequestFinished(InetSocketAddress backendAddress) override;

  private:
    /**
     * @brief Recalculates the internal state required for the WRR algorithm.
     * This includes determining the maximum weight among backends and the
     * greatest common divisor (GCD) of all positive backend weights.
     * This method is called whenever the backend configuration changes.
     */
    void RecalculateWrrState();

    // --- Weighted Round-Robin Specific Member Variables ---
    std::size_t m_currentIndex;  //!< Index of the backend considered in the current selection pass. Initialized to allow first pick.
    int32_t m_currentWeight;     //!< Current weight marker used in the WRR selection algorithm.
    uint32_t m_maxWeight;        //!< Maximum weight among all configured backend servers with positive weight.
    uint32_t m_gcdWeight;        //!< Greatest Common Divisor (GCD) of all positive backend weights.
};

} // namespace ns3
#endif // ROUND_ROBIN_LOAD_BALANCER_H
