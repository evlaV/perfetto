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

#include "src/trace_processor/importers/ftrace/gpuvis_tracker.h"

#include <cstdint>
#include <optional>
#include <sstream>

#include "perfetto/ext/base/string_view.h"
#include "protos/perfetto/trace/ftrace/generic.pbzero.h"
#include "src/trace_processor/importers/common/args_tracker.h"
#include "src/trace_processor/importers/common/process_tracker.h"
#include "src/trace_processor/importers/common/slice_tracker.h"
#include "src/trace_processor/importers/common/track_tracker.h"
#include "src/trace_processor/importers/common/tracks.h"
#include "src/trace_processor/storage/stats.h"
#include "src/trace_processor/types/variadic.h"

namespace perfetto::trace_processor {
namespace {

// We do not expect tracepoints with over 32 fields. It's more likely that the
// trace is corrupted. See also |kMaxFtraceEventFields| in ftrace_descriptors.h.
static constexpr uint32_t kMaxAllowedFields = 32;

using base::StringView;

// // Track blueprints for kernel track events.
// constexpr auto kThreadSliceTrackBp = tracks::SliceBlueprint(
//     "kernel_trackevent_thread_slice",
//     tracks::DimensionBlueprints(
//         tracks::kThreadDimensionBlueprint,
//         tracks::StringIdDimensionBlueprint("tracepoint")),
//     tracks::DynamicNameBlueprint());

}  // namespace

GpuvisTracker::GpuvisTracker(TraceProcessorContext* context)
    : context_(context),
      str_tgid_(context->storage->InternString("tgid")),
      str_func_(context->storage->InternString("func")),
      str_sub_func_(context->storage->InternString("sub_func")),
      str_file_line_(context->storage->InternString("file_line")),
      str_ctx_op_(context->storage->InternString("ctx_op")),
      str_id_(context->storage->InternString("id")),
      str_payload_(context->storage->InternString("payload")),
      str_cpu_(context->storage->InternString("cpu")),
      str_event_type_(context->storage->InternString("event_type")),
      str_gpuvis_userspace_(context->storage->InternString("gpuvis userspace")),
      str_gpuvis_kernel_(context->storage->InternString("gpuvis kernel")),
      str_gpuvis_print_(context->storage->InternString("gpuvis_print")) {}

GpuvisTracker::~GpuvisTracker() = default;

static StringId GetSliceName(TraceProcessorContext* context,
                             std::optional<StringId> func,
                             std::optional<StringId> sub_func) {
  if (!sub_func) {
    return *func;
  }
  std::string combined_name = context->storage->GetString(func).ToStdString() + "." +
                              context->storage->GetString(sub_func).ToStdString();
  return context->storage->InternString(combined_name);
}



static std::pair<std::vector<std::pair<StringView, StringView>>, std::string>
ExtractKeyValuePairs(const base::StringView str) {
  std::vector<std::pair<StringView, StringView>> key_values;
  std::ostringstream leftovers;

  size_t start = 0;

  // Simple space-based tokenizer
  while (start < str.size()) {
    size_t end = str.find(' ', start);
    if (end == StringView::npos)
      end = str.size();

    StringView token = str.substr(start, end - start);
    start = end + 1;  // skip the space

    if (token.empty())
      continue;

    size_t eq_pos = token.find('=');
    if (eq_pos != StringView::npos && eq_pos > 0 && eq_pos < token.size() - 1) {
      // Found a key=value pair
      StringView key = token.substr(0, eq_pos);
      StringView value = token.substr(eq_pos + 1);
      key_values.emplace_back(key, value);
    } else {
      // Not a key=value token, keep it in leftovers
      if (leftovers.tellp() > 0)
        leftovers << ' ';
      leftovers.write(token.data(), static_cast<std::streamsize>(token.size()));
    }
  }

  return {key_values, leftovers.str()};
}

bool GpuvisTracker::TryParseGenericEvent(int64_t ts,
                                         uint32_t cpu,
                                         uint32_t tid,
                                         protozero::ConstBytes blob) {
  protos::pbzero::GenericFtraceEvent::Decoder evt(blob);

  // Check if this is a gpuvis event
  auto event_name_id = context_->storage->InternString(evt.event_name());
  if (event_name_id != str_gpuvis_print_) {
    return false;
  }

  // Parse fields from the event
  std::optional<int64_t> tgid;
  std::optional<StringId> func;
  std::optional<StringId> sub_func;
  std::optional<StringId> file_line;
  std::optional<StringId> ctx_op;
  std::optional<StringId> payload;
  std::optional<uint64_t> id;

  uint32_t field_count = 0;
  for (auto it = evt.field(); it; ++it, ++field_count) {
    if (field_count >= kMaxAllowedFields) {
      PERFETTO_DLOG("Skipping generic descriptor with >32 fields.");
      context_->storage->IncrementStats(
          stats::ftrace_generic_descriptor_errors);
      return false;
    }

    protos::pbzero::GenericFtraceEvent::Field::Decoder field(*it);
    auto name_id = context_->storage->InternString(field.name());

    if (name_id == str_tgid_ && field.has_int_value()) {
      tgid = field.int_value();
    } else if (name_id == str_func_ && field.has_str_value()) {
      func = context_->storage->InternString(field.str_value());
    } else if (name_id == str_sub_func_ && field.has_str_value() &&
               !base::StringView(field.str_value()).empty()) {
      sub_func = context_->storage->InternString(field.str_value());
    } else if (name_id == str_file_line_ && field.has_str_value()) {
      file_line = context_->storage->InternString(field.str_value());
    } else if (name_id == str_ctx_op_ && field.has_str_value()) {
      ctx_op = context_->storage->InternString(field.str_value());
    } else if (name_id == str_payload_ && field.has_str_value()) {
      payload = context_->storage->InternString(field.str_value());
    } else if (name_id == str_id_ && field.has_uint_value()) {
      id = field.uint_value();
    }
  }

  // Validate we have the essential fields.
  if (!func || !ctx_op || !id || !tgid || *tgid == 0) {
    return true;  // It's a gpuvis event, but malformed.
  }

  // Determine if this is a begin or end event.
  auto ctx_op_str = context_->storage->GetString(*ctx_op);
  bool is_begin = ctx_op_str == "begin_ctx", is_end = ctx_op_str == "end_ctx";
  if (!is_begin && !is_end) {
    return true;  // Unknown ctx_op, ignore.
  }

  StringId slice_name = GetSliceName(context_, func, sub_func);
  UniqueTid utid = context_->process_tracker->UpdateThread(tid, *tgid);
  TrackId track_id = context_->track_tracker->InternThreadTrack(utid);

  if (is_begin) {
    // Start a new slice.
    tables::SliceTable::Row row;
    row.ts = ts;
    row.track_id = track_id;
    row.name = slice_name;

    context_->slice_tracker->Begin(
        ts, track_id, str_gpuvis_kernel_, slice_name,
        [&](ArgsTracker::BoundInserter* args) {
          if (tgid) {
            args->AddArg(str_tgid_, Variadic::Integer(*tgid));
          }
          if (sub_func) {
            args->AddArg(str_sub_func_, Variadic::String(*sub_func));
          }
          if (file_line) {
            args->AddArg(str_file_line_, Variadic::String(*file_line));
          }
          if (payload) {
            args->AddArg(str_payload_, Variadic::String(*payload));
          }
          args->AddArg(str_id_, Variadic::UnsignedInteger(*id));
          args->AddArg(str_cpu_, Variadic::UnsignedInteger(cpu));
        });
  } else if (is_end) {
    // End the slice
    context_->slice_tracker->End(ts, track_id);
  }

  return true;
}

bool GpuvisTracker::TryParsePrintEvent(int64_t ts,
                                       uint32_t tid,
                                       base::StringView event) {
  // Quick check for gpuvis print events.
  // Example message "[RunSingleFrame] begin_ctx=53115091 tgid=4411
  // file=/data/src/vrwebhelper/vrwebhelper_javascript.cpp:5023 Graphics"
  if (!event.StartsWith("[")) {
    return false;
  }

  // Parse the event string to extract relevant fields.
  // Where name = "RunSingleFrame", ctx_op = "begin_ctx", id = 53115091, tgid =
  // 4411, file = "/data/src/vrwebhelper/vrwebhelper_javascript.cpp:5023",
  // payload = "Graphics"
  base::StringView name, ctx_op;
  std::optional<uint64_t> id;
  std::optional<uint32_t> tgid;
  base::StringView file_line;

  size_t pos = 0;
  size_t end_bracket = event.find(']');
  if (end_bracket == base::StringView::npos) {
    return false;
  }
  name = event.substr(1, end_bracket - 1);
  pos = end_bracket + 1;

  auto kv_ret = ExtractKeyValuePairs(event.substr(pos));
  auto kv_pairs = kv_ret.first;
  auto payload = kv_ret.second;

  // Extract known key-value pairs
  for (const auto& kv : kv_pairs) {
    auto key = kv.first;
    auto value = kv.second;
    if (key == "begin_ctx" || key == "end_ctx") {
      ctx_op = key;
      id = base::StringViewToUInt64(value);
    } else if (key == "tgid") {
      tgid = base::StringViewToUInt32(value);
    } else if (key == "file") {
      file_line = value;
    }
  }

  // Validate essential fields.  If `begin_ctx` and `end_ctx` are both missing,
  // treat this as an instantaneous point.
  bool is_begin = ctx_op == "begin_ctx", is_end = ctx_op == "end_ctx";
  if (name.empty()) {
    return true;  // It's a gpuvis print event, but malformed.
  }

  // Start a new slice.
  StringId slice_name = context_->storage->InternString(name);
  UniqueTid utid = context_->process_tracker->UpdateThread(tid, *tgid);
  TrackId track_id = context_->track_tracker->InternThreadTrack(utid);

  auto slice_args = [&](ArgsTracker::BoundInserter* args) {
    if (id) {
      args->AddArg(str_id_, Variadic::Integer(static_cast<int64_t>(*id)));
    }
    if (tgid) {
      args->AddArg(str_tgid_, Variadic::Integer(*tgid));
    }
    for (const auto& kv : kv_pairs) {
      auto key = kv.first;
      auto value = kv.second;
      if (key == "begin_ctx" || key == "end_ctx" || key == "tgid")
        continue;

      args->AddArg(
        context_->storage->InternString(key),
        Variadic::String(context_->storage->InternString(value))
      );
    }
  };

  // If it has neither begin nor end, treat it as an instantaenous event
  if (!is_begin && !is_end) {
    context_->slice_tracker->Scoped(ts, track_id, str_gpuvis_userspace_, slice_name, 0, slice_args);
  } else {
    auto ongoing_slice = std::find(ongoing_slices_.begin(), ongoing_slices_.end(), *id);
    if (is_begin) {
      // Ignore begins that already have an ongoing slice
      if (ongoing_slice == ongoing_slices_.end()) {
        context_->slice_tracker->Begin(ts, track_id, str_gpuvis_userspace_, slice_name, slice_args);

        // Record the ongoing slice.
        ongoing_slices_.emplace_back(*id);
      }
    } else { // is_end
      // Ignore ends that don't have an ongoing slice
      if (ongoing_slice != ongoing_slices_.end()) {
        // End the slice.
        context_->slice_tracker->End(ts, track_id);

        // Remove from ongoing slices.
        ongoing_slices_.erase(ongoing_slice);
      }
    }
  }

  return true;
}

}  // namespace perfetto::trace_processor
