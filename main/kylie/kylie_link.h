// KylieAI patch: persistent WebSocket channel + proactive (server-initiated) speech.
// Self-contained supervisor; the upstream code only calls the hooks below.
// See KYLIE-NOTES.md, section "Parche Kylie: canal persistente".
#ifndef KYLIE_LINK_H_
#define KYLIE_LINK_H_

#include <cstdint>

#include "device_state.h"

class Application;

class KylieLink {
public:
    explicit KylieLink(Application& app) : app_(app) {}

    // Activation finished (main task). websocket_selected: OTA chose the WebSocket transport.
    void Start(bool websocket_selected);
    // 1 Hz clock tick (main task): connection supervisor, deferred end of proactive speech.
    void Tick();
    // Wi-Fi up/down events (main task).
    void OnNetwork(bool connected);
    // Incoming "tts start" / "tts stop" handled in the main task. OnTtsStop() returns true when
    // the stop belongs to a proactive speech and has been consumed (caller must not change state).
    void OnTtsStart(DeviceState state_before);
    bool OnTtsStop();
    // True when the board power-save timers must not dim the screen / shut the device down.
    bool KeepAwake() const;

private:
    Application& app_;
    bool enabled_ = false;
    bool network_up_ = true;
    bool was_open_ = false;
    int failures_ = 0;
    int64_t next_attempt_ms_ = 0;
    int64_t open_since_ms_ = 0;

    bool proactive_ = false;
    bool finish_pending_ = false;      // proactive tts stop received, waiting for playback to drain
    int64_t followup_deadline_ms_ = 0;  // 0 = no follow-up listening armed

    void TryConnect(int64_t now_ms);
    void FinishProactive(int64_t now_ms);
    void ScheduleRetry(int64_t now_ms);
};

#endif  // KYLIE_LINK_H_
