// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef MEDIA_BASE_WATCH_TIME_KEYS_H_
#define MEDIA_BASE_WATCH_TIME_KEYS_H_

#include "base/containers/flat_set.h"
#include "base/strings/string_piece.h"
#include "media/base/media_export.h"

namespace media {

// Histogram names used for reporting; also double as MediaLog key names.
// NOTE: If you add to this list you must update GetWatchTimeKeys() and if
// necessary, GetWatchTimePowerKeys().
MEDIA_EXPORT extern const char kWatchTimeAudioAll[];
MEDIA_EXPORT extern const char kWatchTimeAudioMse[];
MEDIA_EXPORT extern const char kWatchTimeAudioEme[];
MEDIA_EXPORT extern const char kWatchTimeAudioSrc[];
MEDIA_EXPORT extern const char kWatchTimeAudioBattery[];
MEDIA_EXPORT extern const char kWatchTimeAudioAc[];
MEDIA_EXPORT extern const char kWatchTimeAudioEmbeddedExperience[];
MEDIA_EXPORT extern const char kWatchTimeAudioVideoAll[];
MEDIA_EXPORT extern const char kWatchTimeAudioVideoMse[];
MEDIA_EXPORT extern const char kWatchTimeAudioVideoEme[];
MEDIA_EXPORT extern const char kWatchTimeAudioVideoSrc[];
MEDIA_EXPORT extern const char kWatchTimeAudioVideoBattery[];
MEDIA_EXPORT extern const char kWatchTimeAudioVideoAc[];
MEDIA_EXPORT extern const char kWatchTimeAudioVideoEmbeddedExperience[];
MEDIA_EXPORT extern const char kWatchTimeAudioVideoBackgroundAll[];
MEDIA_EXPORT extern const char kWatchTimeAudioVideoBackgroundMse[];
MEDIA_EXPORT extern const char kWatchTimeAudioVideoBackgroundEme[];
MEDIA_EXPORT extern const char kWatchTimeAudioVideoBackgroundSrc[];
MEDIA_EXPORT extern const char kWatchTimeAudioVideoBackgroundBattery[];
MEDIA_EXPORT extern const char kWatchTimeAudioVideoBackgroundAc[];
MEDIA_EXPORT extern const char
    kWatchTimeAudioVideoBackgroundEmbeddedExperience[];
// **** If adding any line above this see the toplevel comment! ****

// Markers which signify the watch time should be finalized immediately.
MEDIA_EXPORT extern const char kWatchTimeFinalize[];
MEDIA_EXPORT extern const char kWatchTimeFinalizePower[];

// Count of the number of underflow events during a media session.
MEDIA_EXPORT extern const char kWatchTimeUnderflowCount[];

// UMA keys for MTBR samples.
MEDIA_EXPORT extern const char kMeanTimeBetweenRebuffersAudioSrc[];
MEDIA_EXPORT extern const char kMeanTimeBetweenRebuffersAudioMse[];
MEDIA_EXPORT extern const char kMeanTimeBetweenRebuffersAudioEme[];
MEDIA_EXPORT extern const char kMeanTimeBetweenRebuffersAudioVideoSrc[];
MEDIA_EXPORT extern const char kMeanTimeBetweenRebuffersAudioVideoMse[];
MEDIA_EXPORT extern const char kMeanTimeBetweenRebuffersAudioVideoEme[];

MEDIA_EXPORT base::flat_set<base::StringPiece> GetWatchTimeKeys();
MEDIA_EXPORT base::flat_set<base::StringPiece> GetWatchTimePowerKeys();

}  // namespace media

#endif  // MEDIA_BASE_WATCH_TIME_KEYS_H_
