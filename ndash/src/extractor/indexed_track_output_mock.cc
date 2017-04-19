/*
 * Copyright 2017 Google Inc.
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

#include "extractor/indexed_track_output_mock.h"
#include "media_format.h"

namespace ndash {
namespace extractor {

MockIndexedTrackOutput::MockIndexedTrackOutput() {}
MockIndexedTrackOutput::~MockIndexedTrackOutput() {}

void MockIndexedTrackOutput::GiveFormat(
    std::unique_ptr<const MediaFormat> format) {
  given_format_ = std::move(format);
  GiveFormatMock(given_format_.get());
}

}  // namespace extractor
}  // namespace ndash
