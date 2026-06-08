// Shared, SDK-free types used by recorder, correlator, change_detector, and
// the UI. Kept header-only so they can be linked into the SDK-free test binary
// without dragging in any XPLM symbols.

#pragma once

#include <cstdint>
#include <vector>

namespace xp_sherlock
{

enum class RefType : uint8_t
{
    Int,
    Float,
    Double,
    IntArrayElem,
    FloatArrayElem,
};

inline const char *type_name(RefType t)
{
    switch (t)
    {
    case RefType::Int:            return "Int";
    case RefType::Float:          return "Float";
    case RefType::Double:         return "Double";
    case RefType::IntArrayElem:   return "I[]";
    case RefType::FloatArrayElem: return "F[]";
    }
    return "?";
}

union SampleValue
{
    int32_t i;
    float   f;
    double  d;
};

struct Sample
{
    SampleValue v;
    uint32_t    frame;
    float       t_sec;
};

struct ChangeEvent
{
    SampleValue from;
    SampleValue to;
    uint32_t    frame;
    float       t_sec;
    int8_t      direction; // +1 / -1 / 0
};

struct EventStream
{
    uint32_t                 logical_ref_idx = 0;
    RefType                  type            = RefType::Int;
    std::vector<ChangeEvent> events;
    SampleValue              baseline_value{};
    SampleValue              current_value{};
};

struct Candidate
{
    uint32_t logical_ref_idx     = 0;
    int      pos_count           = 0;
    int      neg_count           = 0;
    bool     bidirectional       = false;
    bool     monotonic_staircase = false;
    int      staircase_steps     = 0;
    float    min_latency_ms      = 0.f; // NaN-safe: 0 means "no anchor available"
    float    median_latency_ms   = 0.f;
    bool     has_latency         = false;
    bool     asymmetric_decay    = false;
    bool     is_writable         = false;
    int      total_events        = 0;
    float    score               = 0.f;
    // For UI display, copied at ranking time:
    RefType     type;
    SampleValue baseline_value{};
    SampleValue current_value{};
    // Min / max value observed across the whole record window. Important for
    // switches that return to their starting state (e.g. ON-OFF-ON ends at
    // the same value it began with, making baseline==current trivially equal
    // even though the ref clearly moved).
    SampleValue min_seen{};
    SampleValue max_seen{};
};

} // namespace xp_sherlock
