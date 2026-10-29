#include "kylie_link.h"

#include <esp_log.h>
#include <esp_timer.h>

#include <algorithm>

#include "application.h"
#include "board.h"
#include "settings.h"

#define TAG "KylieLink"

#ifdef CONFIG_KYLIE_PERSISTENT_CHANNEL
static constexpr bool kPersistent = true;
static constexpr int kMaxBackoffMs = CONFIG_KYLIE_RECONNECT_MAX_BACKOFF_S * 1000;
static constexpr int kFollowupMs = CONFIG_KYLIE_PROACTIVE_FOLLOWUP_S * 1000;
#else
static constexpr bool kPersistent = false;
static constexpr int kMaxBackoffMs = 60000;
static constexpr int kFollowupMs = 0;
#endif
#ifdef CONFIG_KYLIE_KEEP_AWAKE
static constexpr bool kKeepAwake = true;
#else
static constexpr bool kKeepAwake = false;
#endif

// A connection that drops before this much uptime counts as a failure (anti hot-loop).
static constexpr int kStableMs = 30000;
static constexpr int kVoiceExtendMs = 5000;

static int64_t NowMs() { return esp_timer_get_time() / 1000; }

// 1 s, 2 s, 4 s, ... capped at kMaxBackoffMs.
static int BackoffMs(int failures) {
    int64_t delay = 1000;
    for (int i = 0; i < failures && delay < kMaxBackoffMs; i++) {
        delay *= 2;
    }
    return static_cast<int>(std::min<int64_t>(delay, kMaxBackoffMs));
}

bool KylieLink::KeepAwake() const { return enabled_ && kKeepAwake; }

void KylieLink::Start(bool websocket_selected) {
    if (!kPersistent) {
        return;
    }
    Settings settings("websocket", false);
    enabled_ = websocket_selected && !settings.GetString("url").empty();
    ESP_LOGI(TAG, "Persistent channel %s", enabled_ ? "enabled" : "disabled (no websocket config)");
    network_up_ = true;
    was_open_ = false;
    failures_ = 0;
    next_attempt_ms_ = NowMs();
}

void KylieLink::OnNetwork(bool connected) {
    if (!enabled_) {
        return;
    }
    network_up_ = connected;
    if (connected) {
        failures_ = 0;
        next_attempt_ms_ = NowMs() + 1500;  // give DHCP/DNS a moment
    } else if (app_.protocol_ && app_.GetDeviceState() == kDeviceStateIdle &&
               app_.protocol_->IsAudioChannelOpened()) {
        // Stock code only closes the channel for non-idle states.
        app_.protocol_->CloseAudioChannel();
    }
}

void KylieLink::ScheduleRetry(int64_t now_ms) {
    int delay = BackoffMs(failures_);
    next_attempt_ms_ = now_ms + delay;
    ESP_LOGW(TAG, "Channel down, retry in %d ms (failures=%d)", delay, failures_);
}

void KylieLink::TryConnect(int64_t now_ms) {
    ESP_LOGI(TAG, "Opening persistent channel (failures so far: %d)", failures_);
    // Silent: a background retry must not raise the error alert (and sound) on every attempt.
    app_.protocol_->SetSilentErrors(true);
    bool ok = app_.protocol_->OpenAudioChannel();
    app_.protocol_->SetSilentErrors(false);
    now_ms = NowMs();  // Open may have blocked for several seconds
    if (ok) {
        ESP_LOGI(TAG, "Persistent channel open");
        was_open_ = true;
        open_since_ms_ = now_ms;
        // Stale "channel closed" callbacks queued before this point run first (FIFO); re-assert
        // the Wi-Fi power level the open channel expects.
        app_.Schedule([]() {
            Board::GetInstance().SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
        });
    } else {
        failures_++;
        ScheduleRetry(now_ms);
    }
}

void KylieLink::Tick() {
    if (!enabled_ || !app_.protocol_) {
        return;
    }
    const int64_t now_ms = NowMs();
    const DeviceState state = app_.GetDeviceState();

    // --- Follow-up listening after a proactive speech --------------------------------------
    if (followup_deadline_ms_ != 0) {
        if (state != kDeviceStateListening) {
            followup_deadline_ms_ = 0;  // conversation moved on (or user ended it)
        } else if (now_ms >= followup_deadline_ms_) {
            if (app_.audio_service_.IsVoiceDetected()) {
                followup_deadline_ms_ = now_ms + kVoiceExtendMs;  // patient is mid-sentence
            } else {
                followup_deadline_ms_ = 0;
                ESP_LOGI(TAG, "No reply after proactive speech, back to idle");
                app_.protocol_->SendStopListening();
                app_.SetDeviceState(kDeviceStateIdle);
            }
        }
    }

    // --- End of a proactive speech: wait until the speaker queue is empty ------------------
    if (finish_pending_) {
        if (state != kDeviceStateSpeaking) {
            finish_pending_ = false;  // aborted by the user / channel closed
        } else if (app_.audio_service_.IsPlaybackIdle()) {
            finish_pending_ = false;
            FinishProactive(now_ms);
        }
    }

    // --- Connection supervisor (only while idle: never interfere with a conversation) ------
    if (state != kDeviceStateIdle) {
        return;
    }
    if (app_.protocol_->IsAudioChannelOpened()) {
        if (!was_open_) {  // opened by the on-demand path
            was_open_ = true;
            open_since_ms_ = now_ms;
        }
        if (failures_ > 0 && now_ms - open_since_ms_ >= kStableMs) {
            failures_ = 0;
        }
        return;
    }

    if (was_open_) {  // just went down (server closed, 120 s without traffic, user ended chat)
        was_open_ = false;
        failures_ = (now_ms - open_since_ms_ < kStableMs) ? failures_ + 1 : 0;
        // Drop the stale socket now; this also reports the closure once.
        app_.protocol_->CloseAudioChannel();
        ScheduleRetry(now_ms);
        return;
    }
    if (!network_up_ || now_ms < next_attempt_ms_) {
        return;
    }
    TryConnect(now_ms);
}

void KylieLink::OnTtsStart(DeviceState state_before) {
    // Called right before the transition to Speaking. Idle means nobody asked for this speech.
    const bool continuing = finish_pending_ && state_before == kDeviceStateSpeaking;
    proactive_ = enabled_ && (state_before == kDeviceStateIdle || continuing);
    finish_pending_ = false;
    followup_deadline_ms_ = 0;
    if (proactive_) {
        ESP_LOGI(TAG, "Proactive speech from server");
    }
}

bool KylieLink::OnTtsStop() {
    if (!proactive_) {
        return false;
    }
    proactive_ = false;
    finish_pending_ = true;  // Tick() ends it once playback has drained
    return true;
}

void KylieLink::FinishProactive(int64_t now_ms) {
    if (kFollowupMs > 0) {
        app_.SetListeningMode(app_.GetDefaultListeningMode());
        followup_deadline_ms_ = now_ms + kFollowupMs;
    } else {
        app_.SetDeviceState(kDeviceStateIdle);
    }
}
