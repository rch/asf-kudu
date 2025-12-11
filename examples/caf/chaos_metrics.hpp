// Chaos Testing Metrics Collection and Reporting
//
// Provides structures and utilities for collecting RTO/RPO metrics during
// continuous chaos testing of event-sourced actors.
//
// Metrics Hierarchy:
// - Per-Failure Metrics: Individual RTO/RPO measurements
// - Real-Time Stats: Rolling window (last N seconds)
// - Hourly Assessment: Aggregated hour stats with percentiles
// - Daily Assessment: 24-hour aggregate with trends

#pragma once

#include <string>
#include <vector>
#include <map>
#include <chrono>
#include <sstream>
#include <iomanip>
#include <cmath>
#include <algorithm>

namespace chaos_metrics {

// ============================================================================
// Per-Failure Measurement
// ============================================================================

struct FailureEvent {
    std::string entity_id;           // Actor that failed
    std::string failure_type;        // "actor_kill", "message_delay", etc.
    int64_t failure_time_us;         // When failure was injected
    int64_t recovery_complete_us;    // When recovery finished
    int64_t sequence_before_failure; // Last sequence number before failure
    int64_t sequence_after_recovery; // First sequence number after recovery
    bool recovery_successful;        // Did recovery complete?

    // Computed metrics
    int64_t getRTO_ms() const {
        if (recovery_complete_us == 0) return -1;
        return (recovery_complete_us - failure_time_us) / 1000;
    }

    int64_t getRPO_events() const {
        if (!recovery_successful) return -1;
        // RPO = events lost during failure/recovery
        // If sequence_after_recovery = sequence_before_failure + 1, no loss
        // If sequence_after_recovery > sequence_before_failure + 1, loss occurred
        int64_t expected_next = sequence_before_failure + 1;
        if (sequence_after_recovery >= expected_next) {
            return sequence_after_recovery - expected_next;
        }
        return 0;
    }
};

// ============================================================================
// Statistics Helper
// ============================================================================

struct Statistics {
    int64_t count = 0;
    double min = 0.0;
    double max = 0.0;
    double avg = 0.0;
    double p50 = 0.0;  // Median
    double p95 = 0.0;
    double p99 = 0.0;

    // Calculate from vector of values
    static Statistics from(const std::vector<double>& values) {
        Statistics stats;
        if (values.empty()) return stats;

        stats.count = values.size();

        // Sort for percentiles
        std::vector<double> sorted = values;
        std::sort(sorted.begin(), sorted.end());

        stats.min = sorted.front();
        stats.max = sorted.back();

        // Average
        double sum = 0.0;
        for (double v : sorted) sum += v;
        stats.avg = sum / sorted.size();

        // Percentiles
        stats.p50 = percentile(sorted, 0.50);
        stats.p95 = percentile(sorted, 0.95);
        stats.p99 = percentile(sorted, 0.99);

        return stats;
    }

    static double percentile(const std::vector<double>& sorted_values, double p) {
        if (sorted_values.empty()) return 0.0;
        double index = p * (sorted_values.size() - 1);
        size_t lower = static_cast<size_t>(std::floor(index));
        size_t upper = static_cast<size_t>(std::ceil(index));
        double weight = index - lower;
        return sorted_values[lower] * (1.0 - weight) + sorted_values[upper] * weight;
    }

    std::string toJSON() const {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(2);
        oss << "{\"count\":" << count
            << ",\"min\":" << min
            << ",\"max\":" << max
            << ",\"avg\":" << avg
            << ",\"p50\":" << p50
            << ",\"p95\":" << p95
            << ",\"p99\":" << p99
            << "}";
        return oss.str();
    }
};

// ============================================================================
// Real-Time Metrics (rolling window)
// ============================================================================

struct RealTimeMetrics {
    int64_t uptime_seconds = 0;
    int32_t active_actors = 0;
    int32_t total_actors = 0;
    int64_t total_failures_injected = 0;
    int64_t total_recoveries_completed = 0;

    // Recent performance (last N failures)
    std::vector<FailureEvent> recent_failures;  // Keep last 100 failures
    int32_t max_recent_failures = 100;

    // Throughput counters
    int64_t events_processed = 0;
    int64_t commands_processed = 0;
    int64_t snapshots_created = 0;

    void recordFailure(const FailureEvent& failure) {
        recent_failures.push_back(failure);
        if (recent_failures.size() > max_recent_failures) {
            recent_failures.erase(recent_failures.begin());
        }
        total_failures_injected++;
        if (failure.recovery_successful) {
            total_recoveries_completed++;
        }
    }

    Statistics getRTOStats() const {
        std::vector<double> rto_values;
        for (const auto& f : recent_failures) {
            if (f.recovery_successful) {
                rto_values.push_back(static_cast<double>(f.getRTO_ms()));
            }
        }
        return Statistics::from(rto_values);
    }

    Statistics getRPOStats() const {
        std::vector<double> rpo_values;
        for (const auto& f : recent_failures) {
            if (f.recovery_successful) {
                rpo_values.push_back(static_cast<double>(f.getRPO_events()));
            }
        }
        return Statistics::from(rpo_values);
    }

    std::string toConsoleOutput() const {
        auto rto = getRTOStats();
        auto rpo = getRPOStats();

        std::ostringstream oss;
        oss << std::fixed << std::setprecision(1);
        oss << "[Chaos Test] Uptime: " << formatUptime(uptime_seconds)
            << " | Actors: " << active_actors << "/" << total_actors
            << " | Failures: " << total_failures_injected
            << " | RTO p99: " << rto.p99 << "ms"
            << " | RPO avg: " << rpo.avg << " events"
            << " | Throughput: " << (events_processed / (uptime_seconds > 0 ? uptime_seconds : 1)) << " evt/s";
        return oss.str();
    }

private:
    static std::string formatUptime(int64_t seconds) {
        int64_t hours = seconds / 3600;
        int64_t minutes = (seconds % 3600) / 60;
        int64_t secs = seconds % 60;

        std::ostringstream oss;
        if (hours > 0) {
            oss << hours << "h" << minutes << "m";
        } else if (minutes > 0) {
            oss << minutes << "m" << secs << "s";
        } else {
            oss << secs << "s";
        }
        return oss.str();
    }
};

// ============================================================================
// Hourly Self-Assessment
// ============================================================================

struct HourlyAssessment {
    std::string timestamp;  // ISO 8601 format
    int64_t uptime_seconds;
    int32_t total_actors;
    int64_t failures_injected;
    int64_t recoveries_completed;

    Statistics rto_stats;
    Statistics rpo_stats;

    int64_t events_processed;
    int64_t commands_processed;
    int64_t snapshots_created;

    double events_per_second;
    double commands_per_second;

    std::string health_status;  // "EXCELLENT", "GOOD", "DEGRADED", "POOR"

    std::string toJSON() const {
        std::ostringstream oss;
        oss << "{\n"
            << "  \"assessment_type\": \"hourly\",\n"
            << "  \"timestamp\": \"" << timestamp << "\",\n"
            << "  \"uptime_seconds\": " << uptime_seconds << ",\n"
            << "  \"total_actors\": " << total_actors << ",\n"
            << "  \"failures_injected\": " << failures_injected << ",\n"
            << "  \"recoveries_completed\": " << recoveries_completed << ",\n"
            << "  \"rto_stats_ms\": " << rto_stats.toJSON() << ",\n"
            << "  \"rpo_stats_events\": " << rpo_stats.toJSON() << ",\n"
            << "  \"throughput\": {\n"
            << "    \"events_per_second\": " << events_per_second << ",\n"
            << "    \"commands_per_second\": " << commands_per_second << ",\n"
            << "    \"snapshots_created\": " << snapshots_created << "\n"
            << "  },\n"
            << "  \"health\": \"" << health_status << "\"\n"
            << "}\n";
        return oss.str();
    }

    static std::string determineHealth(const Statistics& rto_stats, const Statistics& rpo_stats) {
        // Health criteria:
        // EXCELLENT: RTO p99 < 200ms, RPO avg < 1 event
        // GOOD: RTO p99 < 500ms, RPO avg < 5 events
        // DEGRADED: RTO p99 < 1000ms, RPO avg < 10 events
        // POOR: RTO p99 >= 1000ms or RPO avg >= 10 events

        if (rto_stats.p99 < 200 && rpo_stats.avg < 1.0) {
            return "EXCELLENT";
        } else if (rto_stats.p99 < 500 && rpo_stats.avg < 5.0) {
            return "GOOD";
        } else if (rto_stats.p99 < 1000 && rpo_stats.avg < 10.0) {
            return "DEGRADED";
        } else {
            return "POOR";
        }
    }
};

// ============================================================================
// Daily Self-Assessment
// ============================================================================

struct DailyAssessment {
    std::string timestamp;  // ISO 8601 format
    int64_t uptime_seconds;
    int32_t total_actors;
    int64_t failures_injected;
    int64_t recoveries_completed;

    Statistics rto_stats;
    Statistics rpo_stats;

    int64_t total_events_processed;
    int64_t total_commands_processed;
    int64_t total_snapshots_created;

    double avg_events_per_second;
    double avg_commands_per_second;

    std::vector<std::string> hourly_health_statuses;  // Track health trends
    std::string overall_health;

    std::string toJSON() const {
        std::ostringstream oss;
        oss << "{\n"
            << "  \"assessment_type\": \"daily\",\n"
            << "  \"timestamp\": \"" << timestamp << "\",\n"
            << "  \"uptime_seconds\": " << uptime_seconds << ",\n"
            << "  \"total_actors\": " << total_actors << ",\n"
            << "  \"failures_injected\": " << failures_injected << ",\n"
            << "  \"recoveries_completed\": " << recoveries_completed << ",\n"
            << "  \"rto_stats_ms\": " << rto_stats.toJSON() << ",\n"
            << "  \"rpo_stats_events\": " << rpo_stats.toJSON() << ",\n"
            << "  \"throughput\": {\n"
            << "    \"avg_events_per_second\": " << avg_events_per_second << ",\n"
            << "    \"avg_commands_per_second\": " << avg_commands_per_second << ",\n"
            << "    \"total_snapshots_created\": " << total_snapshots_created << "\n"
            << "  },\n"
            << "  \"health_trend\": [";

        for (size_t i = 0; i < hourly_health_statuses.size(); i++) {
            oss << "\"" << hourly_health_statuses[i] << "\"";
            if (i < hourly_health_statuses.size() - 1) oss << ",";
        }

        oss << "],\n"
            << "  \"overall_health\": \"" << overall_health << "\"\n"
            << "}\n";
        return oss.str();
    }
};

// ============================================================================
// Metrics Collector - Main State Management
// ============================================================================

struct MetricsCollector {
    RealTimeMetrics realtime;
    std::vector<FailureEvent> all_failures;  // All failures for hourly/daily aggregation

    int64_t start_time_us;
    int64_t last_hourly_report_time_us;
    int64_t last_daily_report_time_us;

    MetricsCollector() {
        auto now = std::chrono::steady_clock::now();
        start_time_us = std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count();
        last_hourly_report_time_us = start_time_us;
        last_daily_report_time_us = start_time_us;
    }

    void recordFailure(const FailureEvent& failure) {
        realtime.recordFailure(failure);
        all_failures.push_back(failure);
    }

    void updateUptime() {
        auto now = std::chrono::steady_clock::now();
        int64_t now_us = std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count();
        realtime.uptime_seconds = (now_us - start_time_us) / 1000000;
    }

    bool shouldReportHourly() {
        auto now = std::chrono::steady_clock::now();
        int64_t now_us = std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count();
        int64_t elapsed_us = now_us - last_hourly_report_time_us;
        return elapsed_us >= 3600000000;  // 1 hour in microseconds
    }

    bool shouldReportDaily() {
        auto now = std::chrono::steady_clock::now();
        int64_t now_us = std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count();
        int64_t elapsed_us = now_us - last_daily_report_time_us;
        return elapsed_us >= 86400000000;  // 24 hours in microseconds
    }

    HourlyAssessment generateHourlyReport() {
        last_hourly_report_time_us = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();

        HourlyAssessment report;
        report.timestamp = getCurrentTimestamp();
        report.uptime_seconds = realtime.uptime_seconds;
        report.total_actors = realtime.total_actors;
        report.failures_injected = realtime.total_failures_injected;
        report.recoveries_completed = realtime.total_recoveries_completed;

        report.rto_stats = realtime.getRTOStats();
        report.rpo_stats = realtime.getRPOStats();

        report.events_processed = realtime.events_processed;
        report.commands_processed = realtime.commands_processed;
        report.snapshots_created = realtime.snapshots_created;

        report.events_per_second = realtime.uptime_seconds > 0 ?
            static_cast<double>(realtime.events_processed) / realtime.uptime_seconds : 0.0;
        report.commands_per_second = realtime.uptime_seconds > 0 ?
            static_cast<double>(realtime.commands_processed) / realtime.uptime_seconds : 0.0;

        report.health_status = HourlyAssessment::determineHealth(report.rto_stats, report.rpo_stats);

        return report;
    }

    static std::string getCurrentTimestamp() {
        auto now = std::chrono::system_clock::now();
        auto time_t = std::chrono::system_clock::to_time_t(now);
        std::ostringstream oss;
        oss << std::put_time(std::gmtime(&time_t), "%Y-%m-%dT%H:%M:%SZ");
        return oss.str();
    }
};

} // namespace chaos_metrics
