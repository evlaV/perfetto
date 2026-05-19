/*
 * Copyright (C) 2025 The Android Open Source Project
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

#ifndef SRC_TRACE_PROCESSOR_IMPORTERS_FTRACE_GPUVIS_TRACKER_H_
#define SRC_TRACE_PROCESSOR_IMPORTERS_FTRACE_GPUVIS_TRACKER_H_

#include <deque>

#include "perfetto/protozero/proto_decoder.h"

#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/types/trace_processor_context.h"

namespace perfetto::trace_processor {

class GpuvisTracker {
 public:
  explicit GpuvisTracker(TraceProcessorContext* context);
  GpuvisTracker(const GpuvisTracker&) = delete;
  GpuvisTracker& operator=(const GpuvisTracker&) = delete;
  ~GpuvisTracker();

  bool TryParseGenericEvent(int64_t ts,
                            uint32_t cpu,
                            uint32_t tid,
                            protozero::ConstBytes blob);

  bool TryParsePrintEvent(int64_t ts, uint32_t tid, base::StringView event);

 private:
  TraceProcessorContext* const context_;

  std::deque<uint64_t> ongoing_slices_;

  const StringId str_tgid_;
  const StringId str_func_;
  const StringId str_sub_func_;
  const StringId str_file_line_;
  const StringId str_ctx_op_;
  const StringId str_id_;
  const StringId str_payload_;
  const StringId str_cpu_;
  const StringId str_event_type_;
  const StringId str_gpuvis_userspace_;
  const StringId str_gpuvis_kernel_;
  const StringId str_gpuvis_print_;
};

}  // namespace perfetto::trace_processor

#endif  // SRC_TRACE_PROCESSOR_IMPORTERS_FTRACE_GPUVIS_TRACKER_H_
