
#include <node_api.h>

#include <assert.h>
#include <string>
#include <unordered_map>
#include <functional>

#include <v8.h>
#include <v8-profiler.h>

#include <uv.h>

#define FORMAT_SAMPLED 2
#define FORMAT_RAW 1

#ifndef PROFILER_FORMAT 
#define PROFILER_FORMAT FORMAT_SAMPLED
#endif

#ifndef FORMAT_BENCHMARK
#define FORMAT_BENCHMARK 0
#endif

static const uint8_t kMaxStackDepth(128);
static const float kSamplingFrequency(99.0); // 99 to avoid lockstep sampling
static const float kSamplingHz(1 / kSamplingFrequency);
static const int kSamplingInterval(kSamplingHz * 1e6);
static const v8::CpuProfilingNamingMode kNamingMode(v8::CpuProfilingNamingMode::kDebugNaming);
static const v8::CpuProfilingLoggingMode kDefaultLoggingMode(v8::CpuProfilingLoggingMode::kEagerLogging);

// Allow users to override the default logging mode via env variable. This is useful 
// because sometimes the flow of the profiled program can be to execute many sequential 
// transaction - in that case, it may be preferable to set eager logging to avoid paying the
// high cost of profiling for each individual transaction (one example for this are jest 
// tests when run with --runInBand option).
static const char* kEagerLoggingMode = "eager";
static const char* kLazyLoggingMode = "lazy";

v8::CpuProfilingLoggingMode GetLoggingMode() {
  static const char* logging_mode(getenv("SENTRY_PROFILER_LOGGING_MODE"));

  // most times this wont be set so just bail early
  if (!logging_mode) {
    return kDefaultLoggingMode;
  }

  std::string logging_mode_str(logging_mode);
  // other times it'll likely be set to lazy as eager is the default
  if (logging_mode_str == kLazyLoggingMode) {
    return v8::CpuProfilingLoggingMode::kLazyLogging;
  }
  else if (logging_mode_str == kEagerLoggingMode) {
    return v8::CpuProfilingLoggingMode::kEagerLogging;
  }

  return kDefaultLoggingMode;
}

class SentryProfile;
class Profiler;

enum class ProfileStatus {
  kNotStarted,
  kStarted,
  kStopped,
};


class MeasurementsTicker {
private:
  uv_timer_t timer;
  uint64_t period_ms;
  std::unordered_map<std::string, const std::function<bool(uint64_t, v8::HeapStatistics&)>> heap_listeners;
  std::unordered_map<std::string, const std::function<bool(uint64_t, double)>> cpu_listeners;
  v8::Isolate* isolate;
  v8::HeapStatistics heap_stats;
  uv_cpu_info_t cpu_stats;

public:
  MeasurementsTicker(uv_loop_t* loop) :
    period_ms(100),
    isolate(v8::Isolate::GetCurrent())
  {
    uv_timer_init(loop, &timer);
    timer.data = this;
  }

  static void ticker(uv_timer_t*);
  // Memory listeners
  void heap_callback();
  void add_heap_listener(std::string& profile_id, const std::function<bool(uint64_t, v8::HeapStatistics&)> cb);
  void remove_heap_listener(std::string& profile_id, const std::function<bool(uint64_t, v8::HeapStatistics&)>& cb);

  // CPU listeners
  void cpu_callback();
  void add_cpu_listener(std::string& profile_id, const std::function<bool(uint64_t, double)> cb);
  void remove_cpu_listener(std::string& profile_id, const std::function<bool(uint64_t, double)>& cb);

  size_t listener_count();
};

size_t MeasurementsTicker::listener_count() {
  return heap_listeners.size() + cpu_listeners.size();
}

// Heap tickers
void MeasurementsTicker::heap_callback() {
  isolate->GetHeapStatistics(&heap_stats);
  uint64_t ts = uv_hrtime();

  auto it = heap_listeners.begin();
  while (it != heap_listeners.end()) {
    if (it->second(ts, heap_stats)) {
      it = heap_listeners.erase(it);
    }
    else {
      ++it;
    }
  }
}

void MeasurementsTicker::add_heap_listener(std::string& profile_id, const std::function<bool(uint64_t, v8::HeapStatistics&)> cb) {
  heap_listeners.emplace(profile_id, cb);

  if (listener_count() == 1) {
    uv_timer_set_repeat(&timer, period_ms);
    uv_timer_start(&timer, ticker, 0, period_ms);
  }
}

void MeasurementsTicker::remove_heap_listener(std::string& profile_id, const std::function<bool(uint64_t, v8::HeapStatistics&)>& cb) {
  heap_listeners.erase(profile_id);

  if (listener_count() == 0) {
    uv_timer_stop(&timer);
  }
};

// CPU tickers
void MeasurementsTicker::cpu_callback() {
  uint64_t ts = uv_hrtime();

  uv_cpu_info_t* cpu = &cpu_stats;
  int count;
  int err = uv_cpu_info(&cpu, &count);
  if (err) {
    return;
  }

  uint64_t total = 0;
  uint64_t idle_total = 0;

  for (int i = 0; i < count; i++) {
    uv_cpu_info_t* core = cpu + i;

    total += core->cpu_times.user;
    total += core->cpu_times.nice;
    total += core->cpu_times.sys;
    total += core->cpu_times.idle;
    total += core->cpu_times.irq;

    idle_total += core->cpu_times.idle;
  }

  double total_avg = total / count;
  double idle_avg = idle_total / count;
  double rate = 1.0 - idle_avg / total_avg;

  auto it = cpu_listeners.begin();
  while (it != cpu_listeners.end()) {
    if (it->second(ts, rate)) {
      it = cpu_listeners.erase(it);
    }
    else {
      ++it;
    }
  }

  uv_free_cpu_info(cpu, count);
}

void MeasurementsTicker::ticker(uv_timer_t* handle) {
  MeasurementsTicker* self = static_cast<MeasurementsTicker*>(handle->data);
  self->heap_callback();
  self->cpu_callback();
}

void MeasurementsTicker::add_cpu_listener(std::string& profile_id, const std::function<bool(uint64_t, double)> cb) {
  cpu_listeners.emplace(profile_id, cb);

  if (listener_count() == 1) {
    uv_timer_set_repeat(&timer, period_ms);
    uv_timer_start(&timer, ticker, 0, period_ms);
  }
}

void MeasurementsTicker::remove_cpu_listener(std::string& profile_id, const std::function<bool(uint64_t, double)>& cb) {
  cpu_listeners.erase(profile_id);

  if (listener_count() == 0) {
    uv_timer_stop(&timer);
  }
};

class Profiler {
public:
  std::unordered_map<std::string, SentryProfile*> active_profiles;

  MeasurementsTicker measurements_ticker;
  v8::CpuProfiler* cpu_profiler;

  explicit Profiler(const napi_env& env, v8::Isolate* isolate) :
    measurements_ticker(uv_default_loop()),
    cpu_profiler(
      v8::CpuProfiler::New(isolate, kNamingMode, GetLoggingMode())) {
    napi_add_env_cleanup_hook(env, DeleteInstance, this);
  }

  static void DeleteInstance(void* data);
};

class SentryProfile {
private:
  uint64_t started_at;
  uint16_t heap_write_index = 0;
  uint16_t cpu_write_index = 0;

  std::vector<uint64_t> heap_stats_ts;
  std::vector<uint64_t> heap_stats_usage;

  std::vector<uint64_t> cpu_stats_ts;
  std::vector<double> cpu_stats_usage;


  const std::function<bool(uint64_t, v8::HeapStatistics&)> memory_sampler_cb;
  const std::function<bool(uint64_t, double)> cpu_sampler_cb;

  ProfileStatus status = ProfileStatus::kNotStarted;
  std::string id;

public:
  explicit SentryProfile(const char* id) :
    started_at(uv_hrtime()),
    memory_sampler_cb([this](uint64_t ts, v8::HeapStatistics& stats) {

    if ((heap_write_index >= heap_stats_ts.capacity()) || heap_write_index >= heap_stats_usage.capacity()) {
      return true;
    }

    heap_stats_ts.insert(heap_stats_ts.begin() + heap_write_index, ts - started_at);
    heap_stats_usage.insert(heap_stats_usage.begin() + heap_write_index, static_cast<uint64_t>(stats.used_heap_size()));
    ++heap_write_index;

    return false;
      }),

    cpu_sampler_cb([this](uint64_t ts, double rate) {
    if (cpu_write_index >= cpu_stats_ts.capacity() || cpu_write_index >= cpu_stats_usage.capacity
    ()) {
      return true;
    }
    cpu_stats_ts.insert(cpu_stats_ts.begin() + cpu_write_index, ts - started_at);
    cpu_stats_usage.insert(cpu_stats_usage.begin() + cpu_write_index, rate);
    ++cpu_write_index;
    return false;
      }),

    status(ProfileStatus::kNotStarted),
    id(id) {
    heap_stats_ts.reserve(300);
    heap_stats_usage.reserve(300);
    cpu_stats_ts.reserve(300);
    cpu_stats_usage.reserve(300);
  }

  const std::vector<uint64_t>& heap_usage_timestamps()const;
  const std::vector<uint64_t>& heap_usage_values()const;
  const uint16_t& heap_usage_write_index() const;

  const std::vector<uint64_t>& cpu_usage_timestamps() const;
  const std::vector<double>& cpu_usage_values() const;
  const uint16_t& cpu_usage_write_index() const;

  void Start(Profiler* profiler);
  v8::CpuProfile* Stop(Profiler* profiler);
};


void SentryProfile::Start(Profiler* profiler) {
  v8::Local<v8::String> profile_title = v8::String::NewFromUtf8(v8::Isolate::GetCurrent(), id.c_str(), v8::NewStringType::kNormal).ToLocalChecked();

  started_at = uv_hrtime();

  // Initialize the CPU Profiler
  profiler->cpu_profiler->StartProfiling(profile_title, {
      v8::CpuProfilingMode::kCallerLineNumbers,
      v8::CpuProfilingOptions::kNoSampleLimit,
      kSamplingInterval
    });


  // listen for memory sample ticks
  profiler->measurements_ticker.add_cpu_listener(id, cpu_sampler_cb);
  profiler->measurements_ticker.add_heap_listener(id, memory_sampler_cb);

  status = ProfileStatus::kStarted;
}

static void CleanupSentryProfile(Profiler* profiler, SentryProfile* sentry_profile, const std::string& profile_id) {
  if (sentry_profile == nullptr) {
    return;
  }

  profiler->active_profiles.erase(profile_id);
  delete sentry_profile;
};

void Profiler::DeleteInstance(void* data) {
  Profiler* profiler = static_cast<Profiler*>(data);

  for (auto& profile : profiler->active_profiles) {
    CleanupSentryProfile(profiler, profile.second, profile.first);
  }

  profiler->cpu_profiler->Dispose();
  delete profiler;
}

v8::CpuProfile* SentryProfile::Stop(Profiler* profiler) {
  // Stop the CPU Profiler
  v8::CpuProfile* profile = profiler->cpu_profiler->StopProfiling(v8::String::NewFromUtf8(v8::Isolate::GetCurrent(), id.c_str(), v8::NewStringType::kNormal).ToLocalChecked());

  // Remove the meemory sampler
  profiler->measurements_ticker.remove_heap_listener(id, memory_sampler_cb);
  profiler->measurements_ticker.remove_cpu_listener(id, cpu_sampler_cb);
  // If for some reason stopProfiling was called with an invalid profile title or
  // if that title had somehow been stopped already, profile will be null.
  status = ProfileStatus::kStopped;
  return profile;
}

// Memory getters
const std::vector<uint64_t>& SentryProfile::heap_usage_timestamps() const {
  return heap_stats_ts;
};

const std::vector<uint64_t>& SentryProfile::heap_usage_values() const {
  return heap_stats_usage;
};

const uint16_t& SentryProfile::heap_usage_write_index() const {
  return heap_write_index;
};

// CPU getters
const std::vector<uint64_t>& SentryProfile::cpu_usage_timestamps() const {
  return cpu_stats_ts;
};

const std::vector<double>& SentryProfile::cpu_usage_values() const {
  return cpu_stats_usage;
};
const uint16_t& SentryProfile::cpu_usage_write_index() const {
  return cpu_write_index;
};

#ifdef _WIN32
static const char kPlatformSeparator = '\\';
static const char kWinDiskPrefix = ':';
#else
static const char kPlatformSeparator = '/';
#endif

static const char kSentryPathDelimiter = '.';
static const char kSentryFileDelimiter = ':';
static const std::string kNodeModulesPath = std::string("node_modules") + kPlatformSeparator;

static void GetFrameModule(const std::string& abs_path, std::string& module) {
  if (abs_path.empty()) {
    return;
  }

  module = abs_path;

  // Drop .js extension
  size_t module_len = module.length();
  if (module.compare(module_len - 3, 3, ".js") == 0) {
    module = module.substr(0, module_len - 3);
  }

  // Drop anything before and including node_modules/
  size_t node_modules_pos = module.rfind(kNodeModulesPath);
  if (node_modules_pos != std::string::npos) {
    module = module.substr(node_modules_pos + 13);
  }

  // Replace all path separators with dots except the last one, that one is replaced with a colon
  int match_count = 0;
  for (int pos = module.length() - 1; pos >= 0; pos--) {
    // if there is a match and it's not the first character, replace it
    if (module[pos] == kPlatformSeparator) {
      module[pos] = match_count == 0 ? kSentryFileDelimiter : kSentryPathDelimiter;
      match_count++;
    }
  }

#ifdef _WIN32
  // Strip out C: prefix. On Windows, the drive letter is not part of the module name
  if (module[1] == kWinDiskPrefix) {
    // We will try and strip our the disk prefix.
    module = module.substr(2, std::string::npos);
  }
#endif

  if (module[0] == '.') {
    module = module.substr(1, std::string::npos);
  }
}

static napi_value GetFrameModuleWrapped(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  size_t len;
  assert(napi_get_value_string_utf8(env, argv[0], NULL, 0, &len) == napi_ok);

  char* abs_path = (char*)malloc(len + 1);
  assert(napi_get_value_string_utf8(env, argv[0], abs_path, len + 1, &len) == napi_ok);

  std::string module;
  napi_value napi_module;

  GetFrameModule(abs_path, module);

  assert(napi_create_string_utf8(env, module.c_str(), NAPI_AUTO_LENGTH, &napi_module) == napi_ok);
  return napi_module;
}

napi_value CreateFrameNode(const napi_env& env, const v8::CpuProfileNode& node, std::unordered_map<std::string, std::string>& module_cache, napi_value& resources) {
  napi_value js_node;
  napi_create_object(env, &js_node);

  napi_value lineno_prop;
  napi_create_int32(env, node.GetLineNumber(), &lineno_prop);
  napi_set_named_property(env, js_node, "lineno", lineno_prop);

  napi_value colno_prop;
  napi_create_int32(env, node.GetColumnNumber(), &colno_prop);
  napi_set_named_property(env, js_node, "colno", colno_prop);

  if (node.GetSourceType() != v8::CpuProfileNode::SourceType::kScript) {
    napi_value system_frame_prop;
    napi_get_boolean(env, false, &system_frame_prop);
    napi_set_named_property(env, js_node, "in_app", system_frame_prop);
  }

  napi_value function;
  napi_create_string_utf8(env, node.GetFunctionNameStr(), NAPI_AUTO_LENGTH, &function);
  napi_set_named_property(env, js_node, "function", function);

  const char* resource = node.GetScriptResourceNameStr();

  if (resource != nullptr) {
    // resource is absolute path, set it on the abs_path property
    napi_value abs_path_prop;
    napi_create_string_utf8(env, resource, NAPI_AUTO_LENGTH, &abs_path_prop);
    napi_set_named_property(env, js_node, "abs_path", abs_path_prop);
    // Error stack traces are not relative to root dir, doing our own path normalization
    // breaks people's code mapping configs so we need to leave it as is.
    napi_set_named_property(env, js_node, "filename", abs_path_prop);

    std::string module;
    std::string resource_str = std::string(resource);

    if (resource_str.empty()) {
      return js_node;
    }

    if (module_cache.find(resource_str) != module_cache.end()) {
      module = module_cache[resource_str];
    }
    else {
      napi_value resource;
      napi_create_string_utf8(env, resource_str.c_str(), NAPI_AUTO_LENGTH, &resource);
      napi_set_element(env, resources, module_cache.size(), resource);

      GetFrameModule(resource_str, module);
      module_cache.emplace(resource_str, module);
    }

    if (!module.empty()) {
      napi_value filename_prop;
      napi_create_string_utf8(env, module.c_str(), NAPI_AUTO_LENGTH, &filename_prop);
      napi_set_named_property(env, js_node, "module", filename_prop);
    }
  }

  return js_node;
};


napi_value CreateSample(const napi_env& env, const uint32_t stack_id, const int64_t sample_timestamp_us, const uint32_t thread_id) {
  napi_value js_node;
  napi_create_object(env, &js_node);

  napi_value stack_id_prop;
  napi_create_uint32(env, stack_id, &stack_id_prop);
  napi_set_named_property(env, js_node, "stack_id", stack_id_prop);

  napi_value thread_id_prop;
  napi_create_string_utf8(env, std::to_string(thread_id).c_str(), NAPI_AUTO_LENGTH, &thread_id_prop);
  napi_set_named_property(env, js_node, "thread_id", thread_id_prop);

  napi_value elapsed_since_start_ns_prop;
  napi_create_int64(env, sample_timestamp_us * 1000, &elapsed_since_start_ns_prop);
  napi_set_named_property(env, js_node, "elapsed_since_start_ns", elapsed_since_start_ns_prop);

  return js_node;
};

std::string kDelimiter = std::string(";");
std::string hashCpuProfilerNodeByPath(const v8::CpuProfileNode* node, std::string& path) {
  path.clear();

  while (node != nullptr) {
    path.append(std::to_string(node->GetNodeId()));
    node = node->GetParent();
  }

  return path;
}

static void GetSamples(const napi_env& env, const v8::CpuProfile* profile, const uint32_t thread_id, napi_value& samples, napi_value& stacks, napi_value& frames, napi_value& resources) {
  const int64_t profile_start_time_us = profile->GetStartTime();
  const int sampleCount = profile->GetSamplesCount();

  uint32_t unique_stack_id = 0;
  uint32_t unique_frame_id = 0;

  // Initialize the lookup tables for stacks and frames, both of these are indexed
  // in the sample format we are using to optimize for size.
  std::unordered_map<uint32_t, uint32_t> frame_lookup_table;
  std::unordered_map<std::string, uint32_t> stack_lookup_table;
  std::unordered_map<std::string, std::string> module_cache;

  // At worst, all stacks are unique so reserve the maximum amount of space
  stack_lookup_table.reserve(sampleCount);

  std::string node_hash = "";

  for (int i = 0; i < sampleCount; i++) {
    uint32_t stack_index = unique_stack_id;

    const v8::CpuProfileNode* node = profile->GetSample(i);
    const int64_t sample_timestamp = profile->GetSampleTimestamp(i);

    // If a node was only on top of the stack once, then it will only ever 
    // be inserted once and there is no need for hashing.
    if (node->GetHitCount() > 1) {
      hashCpuProfilerNodeByPath(node, node_hash);

      std::unordered_map<std::string, uint32_t>::iterator stack_index_cache_hit = stack_lookup_table.find(node_hash);

      // If we have a hit, update the stack index, otherwise
      // insert it into the hash table and continue.
      if (stack_index_cache_hit == stack_lookup_table.end()) {
        stack_lookup_table.emplace(node_hash, stack_index);
      }
      else {
        stack_index = stack_index_cache_hit->second;
      }
    }

    napi_value sample = CreateSample(env, stack_index, sample_timestamp - profile_start_time_us, thread_id);

    if (stack_index != unique_stack_id) {
      napi_value index;
      napi_create_uint32(env, i, &index);
      napi_set_property(env, samples, index, sample);
      continue;
    }

    // A stack is a list of frames ordered from outermost (top) to innermost frame (bottom)
    napi_value stack;
    napi_create_array(env, &stack);

    uint32_t stack_depth = 0;

    while (node != nullptr && stack_depth < kMaxStackDepth) {
      auto nodeId = node->GetNodeId();
      auto frame_index = frame_lookup_table.find(nodeId);

      // If the frame does not exist in the index
      if (frame_index == frame_lookup_table.end()) {
        frame_lookup_table.emplace(nodeId, unique_frame_id);

        napi_value frame_id;
        napi_create_uint32(env, unique_frame_id, &frame_id);

        napi_value depth;
        napi_create_uint32(env, stack_depth, &depth);
        napi_set_property(env, stack, depth, frame_id);
        napi_set_property(env, frames, frame_id, CreateFrameNode(env, *node, module_cache, resources));

        unique_frame_id++;
      }
      else {
        // If it was already indexed, just add it's id to the stack
        napi_value depth;
        napi_create_uint32(env, stack_depth, &depth);

        napi_value frame;
        napi_create_uint32(env, frame_index->second, &frame);
        napi_set_property(env, stack, depth, frame);
      };

      // Continue walking down the stack
      node = node->GetParent();
      stack_depth++;
    }

    napi_value napi_sample_index;
    napi_value napi_stack_index;

    napi_create_uint32(env, i, &napi_sample_index);
    napi_set_property(env, samples, napi_sample_index, sample);
    napi_create_uint32(env, stack_index, &napi_stack_index);
    napi_set_property(env, stacks, napi_stack_index, stack);

    unique_stack_id++;
  }
}

static napi_value TranslateMeasurementsDouble(const napi_env& env, const char* unit, const uint16_t size, const std::vector<double>& values, const std::vector<uint64_t>& timestamps) {
  if (size > values.size() || size > timestamps.size()) {
    napi_throw_range_error(env, "NAPI_ERROR", "Memory measurement size is larger than the number of values or timestamps");
    return nullptr;
  }

  if (values.size() != timestamps.size()) {
    napi_throw_range_error(env, "NAPI_ERROR", "Memory measurement entries are corrupt, expected values and timestamps to be of equal length");
    return nullptr;
  }

  napi_value measurement;
  napi_create_object(env, &measurement);

  napi_value unit_string;
  napi_create_string_utf8(env, unit, NAPI_AUTO_LENGTH, &unit_string);
  napi_set_named_property(env, measurement, "unit", unit_string);

  napi_value values_array;
  napi_create_array(env, &values_array);

  for (size_t i = 0; i < size; i++) {
    napi_value entry;
    napi_create_object(env, &entry);

    napi_value value;
    napi_create_double(env, values[i], &value);

    napi_value ts;
    napi_create_int64(env, timestamps[i], &ts);

    napi_set_named_property(env, entry, "value", value);
    napi_set_named_property(env, entry, "elapsed_since_start_ns", ts);
    napi_set_element(env, values_array, i, entry);
  }

  napi_set_named_property(env, measurement, "values", values_array);

  return measurement;
}

static napi_value TranslateMeasurements(const napi_env& env, const char* unit, const uint16_t size, const std::vector<uint64_t>& values, const std::vector<uint64_t>& timestamps) {
  if (size > values.size() || size > timestamps.size()) {
    napi_throw_range_error(env, "NAPI_ERROR", "Memory measurement size is larger than the number of values or timestamps");
    return nullptr;
  }

  if (values.size() != timestamps.size()) {
    napi_throw_range_error(env, "NAPI_ERROR", "Memory measurement entries are corrupt, expected values and timestamps to be of equal length");
    return nullptr;
  }

  napi_value measurement;
  napi_create_object(env, &measurement);

  napi_value unit_string;
  napi_create_string_utf8(env, unit, NAPI_AUTO_LENGTH, &unit_string);
  napi_set_named_property(env, measurement, "unit", unit_string);

  napi_value values_array;
  napi_create_array(env, &values_array);

  for (size_t i = 0; i < size; i++) {
    napi_value entry;
    napi_create_object(env, &entry);

    napi_value value;
    napi_create_int64(env, values[i], &value);

    napi_value ts;
    napi_create_int64(env, timestamps[i], &ts);

    napi_set_named_property(env, entry, "value", value);
    napi_set_named_property(env, entry, "elapsed_since_start_ns", ts);
    napi_set_element(env, values_array, i, entry);
  }

  napi_set_named_property(env, measurement, "values", values_array);

  return measurement;
}

static napi_value TranslateProfile(const napi_env& env, const v8::CpuProfile* profile, const uint32_t thread_id, bool collect_resources) {
  napi_value js_profile;

  napi_create_object(env, &js_profile);

  napi_value logging_mode;
  napi_value samples;
  napi_value stacks;
  napi_value frames;
  napi_value resources;

  napi_create_string_utf8(env, GetLoggingMode() == v8::CpuProfilingLoggingMode::kEagerLogging ? "eager" : "lazy", NAPI_AUTO_LENGTH, &logging_mode);

  napi_create_array(env, &samples);
  napi_create_array(env, &stacks);
  napi_create_array(env, &frames);
  napi_create_array(env, &resources);

  napi_set_named_property(env, js_profile, "samples", samples);
  napi_set_named_property(env, js_profile, "stacks", stacks);
  napi_set_named_property(env, js_profile, "frames", frames);
  napi_set_named_property(env, js_profile, "profiler_logging_mode", logging_mode);

  GetSamples(env, profile, thread_id, samples, stacks, frames, resources);

  if (collect_resources) {
    napi_set_named_property(env, js_profile, "resources", resources);
  }
  else {
    napi_create_array(env, &resources);
    napi_set_named_property(env, js_profile, "resources", resources);
  }

  return js_profile;
}

static napi_value StartProfiling(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];

  assert(napi_get_cb_info(env, info, &argc, argv, NULL, NULL) == napi_ok);

  napi_valuetype callbacktype0;
  assert(napi_typeof(env, argv[0], &callbacktype0) == napi_ok);

  if (callbacktype0 != napi_string) {
    napi_throw_error(env, "NAPI_ERROR", "TypeError: StartProfiling expects a string as first argument.");
    napi_value napi_null;
    assert(napi_get_null(env, &napi_null) == napi_ok);

    return napi_null;
  }

  size_t len;
  assert(napi_get_value_string_utf8(env, argv[0], NULL, 0, &len) == napi_ok);

  char* title = (char*)malloc(len + 1);
  assert(napi_get_value_string_utf8(env, argv[0], title, len + 1, &len) == napi_ok);

  if (len < 1) {
    napi_throw_error(env, "NAPI_ERROR", "StartProfiling expects a non-empty string as first argument, got an empty string.");

    napi_value napi_null;
    assert(napi_get_null(env, &napi_null) == napi_ok);

    return napi_null;
  }

  v8::Isolate* isolate = v8::Isolate::GetCurrent();
  assert(isolate != 0);

  Profiler* profiler;
  assert(napi_get_instance_data(env, (void**)&profiler) == napi_ok);

  if (!profiler) {
    napi_throw_error(env, "NAPI_ERROR", "StartProfiling: Profiler is not initialized.");

    napi_value napi_null;
    assert(napi_get_null(env, &napi_null) == napi_ok);

    return napi_null;
  }

  const std::string profile_id(title);
  // In case we have a collision, cleanup the old profile first
  auto existing_profile = profiler->active_profiles.find(profile_id);
  if (existing_profile != profiler->active_profiles.end()) {
    existing_profile->second->Stop(profiler);
    CleanupSentryProfile(profiler, existing_profile->second, profile_id);
  }

  SentryProfile* sentry_profile = new SentryProfile(title);
  sentry_profile->Start(profiler);

  profiler->active_profiles.emplace(profile_id, sentry_profile);

  napi_value napi_null;
  assert(napi_get_null(env, &napi_null) == napi_ok);

  return napi_null;
}

// StopProfiling(string title)
// https://v8docs.nodesource.com/node-18.2/d2/d34/classv8_1_1_cpu_profiler.html#a40ca4c8a8aa4c9233aa2a2706457cc80
static napi_value StopProfiling(napi_env env, napi_callback_info info) {
  size_t argc = 3;
  napi_value argv[3];

  assert(napi_get_cb_info(env, info, &argc, argv, NULL, NULL) == napi_ok);

  if (argc < 2) {
    napi_throw_error(env, "NAPI_ERROR", "StopProfiling expects at least two arguments.");

    napi_value napi_null;
    assert(napi_get_null(env, &napi_null) == napi_ok);

    return napi_null;
  }

  // Verify the first argument is a string
  napi_valuetype callbacktype0;
  assert(napi_typeof(env, argv[0], &callbacktype0) == napi_ok);

  if (callbacktype0 != napi_string) {
    napi_throw_error(env, "NAPI_ERROR", "StopProfiling expects a string as first argument.");

    napi_value napi_null;
    assert(napi_get_null(env, &napi_null) == napi_ok);

    return napi_null;
  }

  // Verify the second argument is a number
  napi_valuetype callbacktype1;
  assert(napi_typeof(env, argv[1], &callbacktype1) == napi_ok);

  if (callbacktype1 != napi_number) {
    napi_throw_error(env, "NAPI_ERROR", "StopProfiling expects a thread_id integer as second argument.");

    napi_value napi_null;
    assert(napi_get_null(env, &napi_null) == napi_ok);

    return napi_null;
  }

  size_t len;
  assert(napi_get_value_string_utf8(env, argv[0], NULL, 0, &len) == napi_ok);

  char* title = (char*)malloc(len + 1);
  assert(napi_get_value_string_utf8(env, argv[0], title, len + 1, &len) == napi_ok);

  if (len < 1) {
    napi_throw_error(env, "NAPI_ERROR", "StopProfiling expects a string as first argument.");

    napi_value napi_null;
    assert(napi_get_null(env, &napi_null) == napi_ok);

    return napi_null;
  }

  // Get the value of the second argument and convert it to uint64
  int64_t thread_id;
  assert(napi_get_value_int64(env, argv[1], &thread_id) == napi_ok);


  // Get profiler from instance data
  Profiler* profiler;
  assert(napi_get_instance_data(env, (void**)&profiler) == napi_ok);

  if (!profiler) {
    napi_throw_error(env, "NAPI_ERROR", "StopProfiling: Profiler is not initialized.");

    napi_value napi_null;
    assert(napi_get_null(env, &napi_null) == napi_ok);

    return napi_null;
  }

  const std::string profile_id(title);
  auto profile = profiler->active_profiles.find(profile_id);

  // If the profile was never started, silently ignore the call and return null
  if (profile == profiler->active_profiles.end()) {
    napi_value napi_null;
    assert(napi_get_null(env, &napi_null) == napi_ok);
    return napi_null;
  }

  v8::CpuProfile* cpu_profile = profile->second->Stop(profiler);

  // If for some reason stopProfiling was called with an invalid profile title or
  // if that title had somehow been stopped already, profile will be null.
  if (!cpu_profile) {
    CleanupSentryProfile(profiler, profile->second, profile_id);

    napi_value napi_null;
    assert(napi_get_null(env, &napi_null) == napi_ok);
    return napi_null;
  };


  napi_valuetype callbacktype3;
  assert(napi_typeof(env, argv[2], &callbacktype3) == napi_ok);

  bool collect_resources;
  napi_get_value_bool(env, argv[2], &collect_resources);

  napi_value js_profile = TranslateProfile(env, cpu_profile, thread_id, collect_resources);

  napi_value measurements;
  napi_create_object(env, &measurements);

  static const char* memory_unit = "byte";
  napi_value heap_usage_measurements = TranslateMeasurements(env, memory_unit, profile->second->heap_usage_write_index(), profile->second->heap_usage_values(), profile->second->heap_usage_timestamps());

  if (heap_usage_measurements != nullptr) {
    napi_set_named_property(env, measurements, "memory_footprint", heap_usage_measurements);
  }

  static const char* cpu_unit = "percent";
  napi_value cpu_usage_measurements = TranslateMeasurementsDouble(env, cpu_unit, profile->second->cpu_usage_write_index(), profile->second->cpu_usage_values(), profile->second->cpu_usage_timestamps());

  if (cpu_usage_measurements != nullptr) {
    napi_set_named_property(env, measurements, "cpu_usage", cpu_usage_measurements);
  }

  napi_set_named_property(env, js_profile, "measurements", measurements);

  CleanupSentryProfile(profiler, profile->second, profile_id);
  cpu_profile->Delete();

  return js_profile;
};

napi_value Init(napi_env env, napi_value exports) {
  v8::Isolate* isolate = v8::Isolate::GetCurrent();

  if (isolate == nullptr) {
    napi_throw_error(env, nullptr, "Failed to initialize Sentry profiler: isolate is null.");
    return NULL;
  }

  Profiler* profiler = new Profiler(env, isolate);

  if (napi_set_instance_data(env, profiler, NULL, NULL) != napi_ok) {
    napi_throw_error(env, nullptr, "Failed to set instance data for profiler.");
    return NULL;
  }

  napi_value external;
  if (napi_create_external(env, profiler, nullptr, nullptr, &external) != napi_ok) {
    napi_throw_error(env, nullptr, "Failed to create external for profiler instance.");
    return NULL;
  }

  napi_value start_profiling;
  if (napi_create_function(env, "startProfiling", NAPI_AUTO_LENGTH, StartProfiling, external, &start_profiling) != napi_ok) {
    napi_throw_error(env, nullptr, "Failed to create startProfiling function.");
    return NULL;
  }

  if (napi_set_named_property(env, exports, "startProfiling", start_profiling) != napi_ok) {
    napi_throw_error(env, nullptr, "Failed to set startProfiling property on exports.");
    return NULL;
  }

  napi_value stop_profiling;
  if (napi_create_function(env, "stopProfiling", NAPI_AUTO_LENGTH, StopProfiling, external, &stop_profiling) != napi_ok) {
    napi_throw_error(env, nullptr, "Failed to create stopProfiling function.");
    return NULL;
  }

  if (napi_set_named_property(env, exports, "stopProfiling", stop_profiling) != napi_ok) {
    napi_throw_error(env, nullptr, "Failed to set stopProfiling property on exports.");
    return NULL;
  }

  napi_value get_frame_module;
  if (napi_create_function(env, "getFrameModule", NAPI_AUTO_LENGTH, GetFrameModuleWrapped, external, &get_frame_module) != napi_ok) {
    napi_throw_error(env, nullptr, "Failed to create getFrameModule function.");
    return NULL;
  }

  if (napi_set_named_property(env, exports, "getFrameModule", get_frame_module) != napi_ok) {
    napi_throw_error(env, nullptr, "Failed to set getFrameModule property on exports.");
    return NULL;
  }

  return exports;
}

NAPI_MODULE(NODE_GYP_MODULE_NAME, Init)