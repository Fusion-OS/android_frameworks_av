/*
 * Copyright (C) 2018 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <utility>

#include <android-base/macros.h>
#include <media/DrmMetrics.h>
#include <media/stagefright/foundation/base64.h>
#include <sys/time.h>
#include <utils/Timers.h>

using ::android::hardware::drm::V1_0::EventType;
using ::android::hardware::drm::V1_0::KeyStatusType;
using ::android::os::PersistableBundle;

namespace {

template <typename T> std::string GetAttributeName(T type);

template <> std::string GetAttributeName<KeyStatusType>(KeyStatusType type) {
    static const char *type_names[] = {"USABLE", "EXPIRED",
                                       "OUTPUT_NOT_ALLOWED", "STATUS_PENDING",
                                       "INTERNAL_ERROR"};
    if (((size_t)type) > arraysize(type_names)) {
        return "UNKNOWN_TYPE";
    }
    return type_names[(size_t)type];
}

template <> std::string GetAttributeName<EventType>(EventType type) {
    static const char *type_names[] = {"PROVISION_REQUIRED", "KEY_NEEDED",
                                       "KEY_EXPIRED", "VENDOR_DEFINED",
                                       "SESSION_RECLAIMED"};
    if (((size_t)type) > arraysize(type_names)) {
        return "UNKNOWN_TYPE";
    }
    return type_names[(size_t)type];
}

template <typename T>
void ExportCounterMetric(const android::CounterMetric<T> &counter,
                         PersistableBundle *metrics) {
    if (!metrics) {
        ALOGE("metrics was unexpectedly null.");
        return;
    }
    std::string success_count_name = counter.metric_name() + ".ok.count";
    std::string error_count_name = counter.metric_name() + ".error.count";
    std::vector<int64_t> status_values;
    counter.ExportValues(
        [&](const android::status_t status, const int64_t value) {
            if (status == android::OK) {
                metrics->putLong(android::String16(success_count_name.c_str()),
                                 value);
            } else {
                int64_t total_errors(0);
                metrics->getLong(android::String16(error_count_name.c_str()),
                                 &total_errors);
                metrics->putLong(android::String16(error_count_name.c_str()),
                                 total_errors + value);
                status_values.push_back(status);
            }
        });
    if (!status_values.empty()) {
        std::string error_list_name = counter.metric_name() + ".error.list";
        metrics->putLongVector(android::String16(error_list_name.c_str()),
                               status_values);
    }
}

template <typename T>
void ExportCounterMetricWithAttributeNames(
    const android::CounterMetric<T> &counter, PersistableBundle *metrics) {
    if (!metrics) {
        ALOGE("metrics was unexpectedly null.");
        return;
    }
    counter.ExportValues([&](const T &attribute, const int64_t value) {
        std::string name = counter.metric_name() + "." +
                           GetAttributeName(attribute) + ".count";
        metrics->putLong(android::String16(name.c_str()), value);
    });
}

template <typename T>
void ExportEventMetric(const android::EventMetric<T> &event,
                       PersistableBundle *metrics) {
    if (!metrics) {
        ALOGE("metrics was unexpectedly null.");
        return;
    }
    std::string success_count_name = event.metric_name() + ".ok.count";
    std::string error_count_name = event.metric_name() + ".error.count";
    std::string timing_name = event.metric_name() + ".ok.average_time_micros";
    std::vector<int64_t> status_values;
    event.ExportValues([&](const android::status_t &status,
                           const android::EventStatistics &value) {
        if (status == android::OK) {
            metrics->putLong(android::String16(success_count_name.c_str()),
                             value.count);
            metrics->putLong(android::String16(timing_name.c_str()),
                             value.mean);
        } else {
            int64_t total_errors(0);
            metrics->getLong(android::String16(error_count_name.c_str()),
                             &total_errors);
            metrics->putLong(android::String16(error_count_name.c_str()),
                             total_errors + value.count);
            status_values.push_back(status);
        }
    });
    if (!status_values.empty()) {
        std::string error_list_name = event.metric_name() + ".error.list";
        metrics->putLongVector(android::String16(error_list_name.c_str()),
                               status_values);
    }
}

void ExportSessionLifespans(
    const std::map<android::String16, std::pair<int64_t, int64_t>>
        &mSessionLifespans,
    PersistableBundle *metrics) {
    if (!metrics) {
        ALOGE("metrics was unexpectedly null.");
        return;
    }

    if (mSessionLifespans.empty()) {
        return;
    }

    PersistableBundle startTimesBundle;
    PersistableBundle endTimesBundle;
    for (auto it = mSessionLifespans.begin(); it != mSessionLifespans.end();
         it++) {
        startTimesBundle.putLong(android::String16(it->first),
                                 it->second.first);
        endTimesBundle.putLong(android::String16(it->first), it->second.second);
    }
    metrics->putPersistableBundle(
        android::String16("drm.mediadrm.session_start_times_ms"),
        startTimesBundle);
    metrics->putPersistableBundle(
        android::String16("drm.mediadrm.session_end_times_ms"), endTimesBundle);
}

android::String8 ToHexString(const android::Vector<uint8_t> &sessionId) {
    android::String8 result;
    for (size_t i = 0; i < sessionId.size(); i++) {
        result.appendFormat("%02x", sessionId[i]);
    }
    return result;
}

int64_t getCurrentTimeMs() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return ((int64_t)tv.tv_sec * 1000) + ((int64_t)tv.tv_usec / 1000);
}

} // namespace

namespace android {

MediaDrmMetrics::MediaDrmMetrics()
    : mOpenSessionCounter("drm.mediadrm.open_session", "status"),
      mCloseSessionCounter("drm.mediadrm.close_session", "status"),
      mGetKeyRequestTiming("drm.mediadrm.get_key_request", "status"),
      mProvideKeyResponseTiming("drm.mediadrm.provide_key_response", "status"),
      mGetProvisionRequestCounter("drm.mediadrm.get_provision_request",
                                  "status"),
      mProvideProvisionResponseCounter(
          "drm.mediadrm.provide_provision_response", "status"),
      mKeyStatusChangeCounter("drm.mediadrm.key_status_change",
                              "key_status_type"),
      mEventCounter("drm.mediadrm.event", "event_type"),
      mGetDeviceUniqueIdCounter("drm.mediadrm.get_device_unique_id", "status") {
}

void MediaDrmMetrics::SetSessionStart(
    const android::Vector<uint8_t> &sessionId) {
    String16 sessionIdHex = String16(ToHexString(sessionId));
    mSessionLifespans[sessionIdHex] =
        std::make_pair(getCurrentTimeMs(), (int64_t)0);
}

void MediaDrmMetrics::SetSessionEnd(const android::Vector<uint8_t> &sessionId) {
    String16 sessionIdHex = String16(ToHexString(sessionId));
    int64_t endTimeMs = getCurrentTimeMs();
    if (mSessionLifespans.find(sessionIdHex) != mSessionLifespans.end()) {
        mSessionLifespans[sessionIdHex] =
            std::make_pair(mSessionLifespans[sessionIdHex].first, endTimeMs);
    } else {
        mSessionLifespans[sessionIdHex] = std::make_pair((int64_t)0, endTimeMs);
    }
}

void MediaDrmMetrics::Export(PersistableBundle *metrics) {
    if (!metrics) {
        ALOGE("metrics was unexpectedly null.");
        return;
    }
    ExportCounterMetric(mOpenSessionCounter, metrics);
    ExportCounterMetric(mCloseSessionCounter, metrics);
    ExportEventMetric(mGetKeyRequestTiming, metrics);
    ExportEventMetric(mProvideKeyResponseTiming, metrics);
    ExportCounterMetric(mGetProvisionRequestCounter, metrics);
    ExportCounterMetric(mProvideProvisionResponseCounter, metrics);
    ExportCounterMetricWithAttributeNames(mKeyStatusChangeCounter, metrics);
    ExportCounterMetricWithAttributeNames(mEventCounter, metrics);
    ExportCounterMetric(mGetDeviceUniqueIdCounter, metrics);
    ExportSessionLifespans(mSessionLifespans, metrics);
}

} // namespace android