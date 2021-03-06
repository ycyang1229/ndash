// Copyright 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "mp4/media_track.h"

namespace media {

MediaTrack::MediaTrack(Type type,
                       const std::string& id,
                       const std::string& kind,
                       const std::string& label,
                       const std::string& lang)
    : type_(type), id_(id), kind_(kind), label_(label), language_(lang) {}

MediaTrack::~MediaTrack() {}

}  // namespace media
