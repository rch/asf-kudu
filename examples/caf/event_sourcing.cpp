// Event Sourcing Implementation

#include "event_sourcing.hpp"
#include <sstream>
#include <iomanip>
#include <cmath>

namespace event_sourcing {

// ============================================================================
// Event JSON Serialization
// ============================================================================

std::string TelemetryReported::toJSON() const {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(2);
    oss << "{"
        << "\"vibration\":" << vibration_rms << ","
        << "\"temperature\":" << temperature_c << ","
        << "\"current\":" << motor_current_a << ","
        << "\"speed\":" << spindle_speed_rpm << ","
        << "\"tool_wear\":" << tool_wear_mm << ","
        << "\"cycles\":" << cycle_count
        << "}";
    return oss.str();
}

std::string EquipmentStateChanged::toJSON() const {
    std::ostringstream oss;
    oss << "{"
        << "\"from\":\"" << from_state << "\","
        << "\"to\":\"" << to_state << "\","
        << "\"reason\":\"" << reason << "\""
        << "}";
    return oss.str();
}

std::string DegradationDetected::toJSON() const {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(1);
    oss << "{"
        << "\"component\":\"" << component << "\","
        << "\"degradation_pct\":" << degradation_pct << ","
        << "\"mtbf_hours\":" << predicted_mtbf_hours << ","
        << "\"model\":\"" << ml_model_version << "\""
        << "}";
    return oss.str();
}

std::string MaintenanceScheduled::toJSON() const {
    std::ostringstream oss;
    oss << "{"
        << "\"scheduled_time\":" << scheduled_time_us << ","
        << "\"type\":\"" << maintenance_type << "\","
        << "\"reason\":\"" << reason << "\""
        << "}";
    return oss.str();
}

std::string MaintenanceCompleted::toJSON() const {
    std::ostringstream oss;
    oss << "{"
        << "\"work\":\"" << work_performed << "\","
        << "\"duration_min\":" << duration_minutes
        << "}";
    return oss.str();
}

// ============================================================================
// Equipment Health State - Deterministic Event Application
// ============================================================================

void EquipmentHealthState::apply(const Event& event) {
    // Update sequence (must be monotonically increasing)
    if (event.sequence <= last_sequence) {
        // Skip duplicate or out-of-order event (idempotency)
        return;
    }
    last_sequence = event.sequence;

    // Apply event based on type (deterministic state transitions)
    if (event.event_type == "TelemetryReported") {
        const auto& telem = static_cast<const TelemetryReported&>(event);

        // Update telemetry statistics
        telemetry_count++;
        max_vibration_rms = std::max(max_vibration_rms, telem.vibration_rms);
        max_temperature_c = std::max(max_temperature_c, telem.temperature_c);

        // Running average for current
        avg_motor_current_a = ((avg_motor_current_a * (telemetry_count - 1)) + telem.motor_current_a) / telemetry_count;

        tool_wear_mm = telem.tool_wear_mm;
        total_cycles = telem.cycle_count;

        // Simple health rules (in production, this would be ML-driven)
        if (telem.vibration_rms > 10.0f && current_state == EquipmentState::HEALTHY) {
            current_state = EquipmentState::DEGRADING;
        }
        if (telem.temperature_c > 85.0f && current_state == EquipmentState::DEGRADING) {
            current_state = EquipmentState::CRITICAL;
        }
    }
    else if (event.event_type == "EquipmentStateChanged") {
        const auto& state_change = static_cast<const EquipmentStateChanged&>(event);
        current_state = stringToState(state_change.to_state);
    }
    else if (event.event_type == "DegradationDetected") {
        const auto& deg = static_cast<const DegradationDetected&>(event);
        degradation_pct = deg.degradation_pct;
        predicted_mtbf_hours = deg.predicted_mtbf_hours;

        // Transition to DEGRADING or CRITICAL based on degradation level
        if (deg.degradation_pct > 80.0f) {
            current_state = EquipmentState::CRITICAL;
        } else if (deg.degradation_pct > 50.0f) {
            current_state = EquipmentState::DEGRADING;
        }
    }
    else if (event.event_type == "MaintenanceScheduled") {
        const auto& maint = static_cast<const MaintenanceScheduled&>(event);
        maintenance_scheduled_time = maint.scheduled_time_us;
    }
    else if (event.event_type == "MaintenanceCompleted") {
        maintenance_count++;
        maintenance_scheduled_time.reset();

        // Reset health metrics after maintenance
        current_state = EquipmentState::CALIBRATING;
        degradation_pct.reset();
        predicted_mtbf_hours.reset();
        max_vibration_rms = 0.0f;
        max_temperature_c = 0.0f;
        tool_wear_mm = 0.0f;
    }
}

// ============================================================================
// Snapshot Serialization (for fast recovery)
// ============================================================================

std::string EquipmentHealthState::toSnapshotJSON() const {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(2);
    oss << "{"
        << "\"equipment_id\":\"" << equipment_id << "\","
        << "\"state\":\"" << stateToString(current_state) << "\","
        << "\"sequence\":" << last_sequence << ","
        << "\"max_vibration\":" << max_vibration_rms << ","
        << "\"max_temperature\":" << max_temperature_c << ","
        << "\"avg_current\":" << avg_motor_current_a << ","
        << "\"tool_wear\":" << tool_wear_mm << ","
        << "\"total_cycles\":" << total_cycles << ","
        << "\"telemetry_count\":" << telemetry_count << ","
        << "\"degradation_pct\":" << (degradation_pct ? *degradation_pct : -1.0f) << ","
        << "\"mtbf_hours\":" << (predicted_mtbf_hours ? *predicted_mtbf_hours : -1.0f) << ","
        << "\"maintenance_count\":" << maintenance_count << ","
        << "\"snapshot_seq\":" << snapshot_sequence << ","
        << "\"snapshot_ts\":" << snapshot_timestamp_us
        << "}";
    return oss.str();
}

EquipmentHealthState EquipmentHealthState::fromSnapshotJSON(const std::string& json) {
    // Simple JSON parsing (in production, use a proper JSON library)
    EquipmentHealthState state;

    // Extract equipment_id
    size_t pos = json.find("\"equipment_id\":\"");
    if (pos != std::string::npos) {
        pos += 16;
        size_t end = json.find("\"", pos);
        state.equipment_id = json.substr(pos, end - pos);
    }

    // Extract state
    pos = json.find("\"state\":\"");
    if (pos != std::string::npos) {
        pos += 9;
        size_t end = json.find("\"", pos);
        state.current_state = stringToState(json.substr(pos, end - pos));
    }

    // Extract sequence
    pos = json.find("\"sequence\":");
    if (pos != std::string::npos) {
        pos += 11;
        state.last_sequence = std::stoll(json.substr(pos));
    }

    // Extract numeric fields (simplified - production would be more robust)
    auto extractFloat = [&json](const std::string& field) -> float {
        size_t pos = json.find("\"" + field + "\":");
        if (pos != std::string::npos) {
            pos += field.length() + 3;
            return std::stof(json.substr(pos));
        }
        return 0.0f;
    };

    state.max_vibration_rms = extractFloat("max_vibration");
    state.max_temperature_c = extractFloat("max_temperature");
    state.avg_motor_current_a = extractFloat("avg_current");
    state.tool_wear_mm = extractFloat("tool_wear");

    float deg = extractFloat("degradation_pct");
    if (deg >= 0.0f) state.degradation_pct = deg;

    float mtbf = extractFloat("mtbf_hours");
    if (mtbf >= 0.0f) state.predicted_mtbf_hours = mtbf;

    return state;
}

} // namespace event_sourcing
