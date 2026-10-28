#include "ContinuousFeedbackController.h"
#include <algorithm>
#include <cmath>

namespace mixsense
{
ContinuousFeedbackController::ContinuousFeedbackController (ControllerBridge& controllerBridge, FeedbackAddress endpoint)
    : bridge (controllerBridge), address (endpoint), session (bridge.sessionGeneration()) {}

bool ContinuousFeedbackController::configure (Settings next)
{
    if (next.refreshInterval <= Clock::duration::zero() || next.sourceMaxAge <= Clock::duration::zero()) return false;
    settings = next;
    return true;
}

void ContinuousFeedbackController::synchronizeSession()
{
    const auto current = bridge.sessionGeneration();
    if (current == session && bridge.isConnected()) return;
    session = current;
    level.reset(); lastAccepted.reset();
    dirty = zeroPending = false; // No automatic replay or stop command in a new session.
}

bool ContinuousFeedbackController::publish (float normalized, bool valid, std::uint64_t generation, Clock::time_point now)
{
    synchronizeSession();
    if (! valid || ! std::isfinite (normalized))
    {
        if (! lastGeneration || generation > *lastGeneration) lastGeneration = generation;
        clear (now);
        return false;
    }
    if (lastGeneration && generation <= *lastGeneration) return false;
    lastGeneration = generation;
    if (! settings || ! bridge.isConnected() || address.target != FeedbackTarget::Haptic) return false;
    const float next = std::clamp (normalized, 0.0f, 1.0f);
    if (next == 0.0f) { clear (now); return true; }
    dirty = dirty || ! level || *level != next;
    level = next; sourceTime = now; zeroPending = false;
    service (now); // Prompt local submission; never device I/O.
    return true;
}

void ContinuousFeedbackController::clear (Clock::time_point now)
{
    synchronizeSession();
    level.reset(); lastAccepted.reset(); dirty = false;
    zeroPending = bridge.isConnected();
    service (now);
}

void ContinuousFeedbackController::service (Clock::time_point now)
{
    synchronizeSession();
    if (! bridge.isConnected()) return;
    if (level && settings && now - sourceTime >= settings->sourceMaxAge)
    {
        level.reset(); lastAccepted.reset(); dirty = false;
        zeroPending = true;
    }
    if (zeroPending)
    {
        if (bridge.submitFeedback ({ address, FeedbackLevel { 0.0f } })) zeroPending = false;
        return;
    }
    if (level && settings && (dirty || ! lastAccepted || now - *lastAccepted >= settings->refreshInterval))
    {
        if (bridge.submitFeedback ({ address, FeedbackLevel { *level } }))
        { lastAccepted = now; dirty = false; }
    }
}
}
