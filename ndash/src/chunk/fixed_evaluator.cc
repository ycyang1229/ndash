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

#include "chunk/fixed_evaluator.h"

#include <deque>
#include <memory>
#include <vector>

#include "base/logging.h"
#include "base/time/time.h"
#include "chunk/chunk.h"
#include "chunk/format_evaluator.h"
#include "util/format.h"

namespace ndash {
namespace chunk {

FixedEvaluator::FixedEvaluator() {}
FixedEvaluator::~FixedEvaluator() {}

void FixedEvaluator::Enable() {}
void FixedEvaluator::Disable() {}

void FixedEvaluator::Evaluate(
    const std::deque<std::unique_ptr<MediaChunk>>& queue,
    base::TimeDelta playback_position,
    const std::vector<util::Format>& formats,
    FormatEvaluation* evaluation,
    const PlaybackRate& playback_rate) const {
  // TODO(adewhurst): Should probably do something with playback_rate
  DCHECK(!formats.empty());
  evaluation->format_.reset(new util::Format(formats.at(0)));
}

}  // namespace chunk
}  // namespace ndash
