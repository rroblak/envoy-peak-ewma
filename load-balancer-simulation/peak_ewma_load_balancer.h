#ifndef PEAK_EWMA_LOAD_BALANCER_H
#define PEAK_EWMA_LOAD_BALANCER_H

// Base class
#include "load_balancer.h"

// NS-3 Includes
#include "ns3/nstime.h"                 // For ns3::Time, Simulator::Now()
#include "ns3/random-variable-stream.h" // For Ptr<UniformRandomVariable>
#include "ns3/simulator.h"              // For Simulator::Now() (already in nstime.h but explicit)

// Standard Library Includes
#include <vector>
#include <map>
#include <string>    // For potential use, though not directly visible here
#include <cmath>     // For std::exp, std::max
#include <limits>    // For std::numeric_limits
#include <utility>   // For std::pair, std::piecewise_construct, std::forward_as_tuple
#include <cstdint>   // For int64_t, uint32_t, uint64_t

namespace ns3 {

// Forward declarations
class Packet;
class Address;
class InetSocketAddress;

/**
 * @brief Holds the Peak EWMA (Exponentially Weighted Moving Average) state for a single backend.
 *
 * This class tracks the latency of a backend server using an EWMA that is sensitive to peaks.
 * It also maintains a count of pending requests. The load score is a combination of the
 * EWMA latency and the number of pending requests.
 */
class EwmaMetric {
public:
    /**
     * @brief Constructs an EwmaMetric.
     * @param decayTime The time window over which the EWMA should decay.
     */
    EwmaMetric(Time decayTime) :
        m_stampNs(Simulator::Now().GetNanoSeconds()),
        m_pending(0),
        m_costNs(0.0), // Initialize cost to 0, implying it's an unknown/idle state
        m_decayTimeNs(std::max(INT64_C(1), decayTime.GetNanoSeconds())), // Ensure positive decay
        // Default penalty: 1 second RTT, used if cost is zero (e.g. new backend or after a peak reset)
        m_penaltyNs(static_cast<double>(Time(Seconds(1.0)).GetNanoSeconds()))
    {
        m_decayTimeNsDouble = static_cast<double>(m_decayTimeNs);
    }

    // Copy constructor
    EwmaMetric(const EwmaMetric& other) :
        m_stampNs(other.m_stampNs), // Copy last update time
        m_pending(other.m_pending),
        m_costNs(other.m_costNs),
        m_decayTimeNs(other.m_decayTimeNs),
        m_decayTimeNsDouble(other.m_decayTimeNsDouble),
        m_penaltyNs(other.m_penaltyNs)
    { }

    // Assignment operator
    EwmaMetric& operator=(const EwmaMetric& other) {
        if (this != &other) {
            m_stampNs = other.m_stampNs;
            m_pending = other.m_pending;
            m_costNs = other.m_costNs;
            m_decayTimeNs = other.m_decayTimeNs;
            m_decayTimeNsDouble = other.m_decayTimeNsDouble;
            m_penaltyNs = other.m_penaltyNs;
        }
        return *this;
    }

    /**
     * @brief Observes a new round-trip time (RTT) measurement and updates the EWMA cost.
     * If the new RTT is significantly higher than the current EWMA (a peak),
     * the EWMA cost is reset to be more responsive to this peak.
     * @param rttNs The RTT measurement in nanoseconds.
     */
    void Observe(int64_t rttNs) {
        int64_t nowNs = Simulator::Now().GetNanoSeconds();
        int64_t tdiff = std::max(INT64_C(0), nowNs - m_stampNs); // Time since last update
        m_stampNs = nowNs;

        // Peak sensitivity: if new RTT is a peak and cost was non-zero, reset cost.
        // This makes the algorithm more reactive to sudden increases in latency.
        if (static_cast<double>(rttNs) > m_costNs && m_costNs > std::numeric_limits<double>::epsilon()) {
            m_costNs = 0.0; // Resetting cost makes the penalty logic in GetLoad apply
        }

        double w = std::exp(-static_cast<double>(tdiff) / m_decayTimeNsDouble); // EWMA weight
        m_costNs = m_costNs * w + static_cast<double>(rttNs) * (1.0 - w); // Update EWMA
    }

    /**
     * @brief Calculates and returns the current load score for this backend.
     * The load score is `ewma_latency * (pending_requests + 1)`.
     * If EWMA latency is zero (e.g., new or reset), a penalty is applied.
     * @return The load score (a higher value indicates a more loaded or latent backend).
     */
    double GetLoad() {
        int64_t nowNs = Simulator::Now().GetNanoSeconds();
        int64_t tdiff = std::max(INT64_C(0), nowNs - m_stampNs);
        if (tdiff > 0) { // Apply decay if time has passed since last update/observation
            double w = std::exp(-static_cast<double>(tdiff) / m_decayTimeNsDouble);
            m_costNs = m_costNs * w;
            m_stampNs = nowNs;
        }

        uint32_t currentPending = m_pending;
        double loadScore;

        // If EWMA cost is effectively zero (e.g., idle or just reset after a peak),
        // apply a penalty plus current pending requests.
        // This helps avoid dog-piling on an idle server or one that just experienced a spike.
        if (m_costNs <= std::numeric_limits<double>::epsilon() && currentPending > 0) {
            loadScore = m_penaltyNs + static_cast<double>(currentPending);
        } else {
            loadScore = m_costNs * static_cast<double>(currentPending + 1);
        }
        return std::max(0.0, loadScore); // Ensure load is not negative
    }

    void IncrementPending() {
        m_pending++;
    }

    void DecrementPending() {
        if (m_pending > 0) {
            m_pending--;
        } else {
            // This indicates a logical error: more decrements than increments.
            // m_pending remains 0 due to unsigned arithmetic with fetch_sub if it was 0.
            // If fetch_sub was used on 0, it would wrap. This logic prevents wrap.
            // NS_LOG_WARN("EwmaMetric: Attempted to decrement pending requests when count was already zero.");
        }
    }

    /** * @brief Gets the current number of pending requests (for debugging/stats).
     * @return Current pending request count.
     */
    uint32_t GetPendingRequests() const {
        return m_pending;
    }

    /** * @brief Gets the current EWMA cost in nanoseconds (for debugging/stats).
     * @return Current EWMA cost.
     */
    double GetCurrentCostNs() const {
        return m_costNs;
    }

private:
    int64_t m_stampNs;               //!< Timestamp of the last observation or update (nanoseconds).
    uint32_t m_pending;              //!< Number of outstanding/pending requests to this backend.
    double m_costNs;                 //!< EWMA of latency in nanoseconds.
    int64_t m_decayTimeNs;           //!< Decay time window in nanoseconds.
    double m_decayTimeNsDouble;      //!< Cached double representation of m_decayTimeNs for performance.
    double m_penaltyNs;              //!< Penalty cost applied when m_costNs is zero (nanoseconds).
};


/**
 * @brief Implements Peak EWMA (Exponentially Weighted Moving Average) load balancing.
 *
 * This load balancer uses the "Power of Two Choices" (P2C) selection strategy.
 * It randomly selects two backend servers and chooses the one with the lower
 * "load score". The load score for each backend is determined by its `EwmaMetric`,
 * which combines an EWMA of its request latency (sensitive to peaks) and the
 * number of its currently pending requests.
 */
class PeakEwmaLoadBalancer : public LoadBalancerApp
{
public:
    static TypeId GetTypeId();
    PeakEwmaLoadBalancer();
    virtual ~PeakEwmaLoadBalancer() override;

    // Override backend management methods to initialize/update EwmaMetric for each backend
    virtual void SetBackends(const std::vector<std::pair<InetSocketAddress, uint32_t>>& backends) override;
    virtual void AddBackend(const InetSocketAddress& backendAddress, uint32_t weight) override;
    virtual void AddBackend(const InetSocketAddress& backendAddress) override; // Adds with default weight

protected:
    /**
     * @brief Chooses a backend using P2C based on the Peak EWMA load metric.
     * @param packet The incoming request packet (content not used by this algorithm).
     * @param fromAddress The source address of the request (not used by this algorithm).
     * @param l7Identifier The L7 identifier from the request (not used by this algorithm).
     * @param[out] chosenBackend The InetSocketAddress of the selected backend server.
     * @return True if a backend was successfully chosen, false otherwise.
     */
    virtual bool ChooseBackend(Ptr<Packet> packet,
                               const Address& fromAddress,
                               uint64_t l7Identifier,
                               InetSocketAddress& chosenBackend) override;

    // Override notification methods from LoadBalancerApp to update EwmaMetrics
    virtual void RecordBackendLatency(InetSocketAddress backendAddress, Time rtt) override;
    virtual void NotifyRequestSent(InetSocketAddress backendAddress) override;
    virtual void NotifyRequestFinished(InetSocketAddress backendAddress) override;

private:
    Time m_decayTime; //!< Configurable decay time for EWMA calculations (attribute).
    Ptr<UniformRandomVariable> m_randomGenerator; //!< RNG for P2C selection.

    //! Map storing EwmaMetric for each backend server, keyed by address.
    std::map<InetSocketAddress, EwmaMetric> m_backendMetrics;
};

} // namespace ns3
#endif // PEAK_EWMA_LOAD_BALANCER_H
