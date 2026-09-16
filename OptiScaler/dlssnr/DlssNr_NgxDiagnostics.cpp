#include "pch.h"
#include "DlssNr_NgxDiagnostics.h"
#include <Logger.h>
#include <atomic>
#include <mutex>

namespace DlssNr::NgxDiagnostics
{
namespace
{
std::atomic_uint active { 0 }, lines { 0 };
std::mutex sinkMutex;
NVSDK_NGX_LoggingInfo original {};
void NVSDK_CONV Callback(const char* message, NVSDK_NGX_Logging_Level level, NVSDK_NGX_Feature feature)
{
    if (!message) return;
    // NVIDIA may report feature creation from a worker thread. Bound each capture window.
    if (active.load() && lines.fetch_add(1) < 512)
        LOG_INFO("NR diagnostic NGX [feature={} level={} thread={}]: {}", (unsigned)feature,
                 (unsigned)level, GetCurrentThreadId(), message);
    NVSDK_NGX_LoggingInfo sink;
    { std::lock_guard lock(sinkMutex); sink = original; }
    static thread_local bool forwarding = false;
    if (!forwarding && sink.LoggingCallback && sink.LoggingCallback != Callback &&
        sink.MinimumLoggingLevel != NVSDK_NGX_LOGGING_LEVEL_OFF && level <= sink.MinimumLoggingLevel)
    {
        forwarding = true;
        sink.LoggingCallback(message, level, feature);
        forwarding = false;
    }
}
} // namespace

void Install(NVSDK_NGX_LoggingInfo& logging)
{
    std::lock_guard lock(sinkMutex);
    if (logging.LoggingCallback != Callback) original = logging;
    logging.LoggingCallback = Callback;
    logging.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_VERBOSE;
    // Leave DisableOtherLoggingSinks as the game supplied it.
}
Scope::Scope() { if (active.fetch_add(1) == 0) lines = 0; }
Scope::~Scope()
{
    if (active.fetch_sub(1) == 1 && lines.load() > 512)
        LOG_INFO("NR diagnostic: NGX capture limited to 512 lines for this initialization window");
}
} // namespace DlssNr::NgxDiagnostics
