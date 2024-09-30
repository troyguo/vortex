// Copyright © 2019-2023
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "scope.h"
#include <VX_config.h>
#include <nlohmann_json.hpp>
#include <iostream>
#include <fstream>
#include <thread>
#include <chrono>
#include <vector>
#include <list>
#include <assert.h>
#include <chrono>
#include <thread>
#include <condition_variable>
#include <mutex>
#include <unordered_set>
#include <sstream>

#define SAMPLE_FLUSH_SIZE 100

#define TIMEOUT_TIME (60*60)

#define MAX_DELAY_CYCLES 10000

#define MMIO_SCOPE_READ  (AFU_IMAGE_MMIO_SCOPE_READ * 4)
#define MMIO_SCOPE_WRITE (AFU_IMAGE_MMIO_SCOPE_WRITE * 4)

#define CMD_GET_WIDTH   0
#define CMD_GET_COUNT   1
#define CMD_GET_START   2
#define CMD_GET_DATA    3
#define CMD_SET_START   4
#define CMD_SET_STOP    5
#define CMD_SET_DEPTH   6

#define CHECK_ERR(_expr)    \
  do {                      \
    int err = _expr;        \
    if (err == 0)           \
      break;                \
    printf("[SCOPE] error: '%s' returned %d!\n", #_expr, err); \
    return err;             \
  } while (false)

struct tap_signal_t {
  uint32_t id;
  std::string name;
  uint32_t width;
};

struct tap_t {
  uint32_t id;
  uint32_t width;
  uint32_t samples;
  uint32_t cur_sample;
  uint64_t cycle_time;
  std::string path;
  std::vector<tap_signal_t> signals;
};

static scope_callback_t g_callback;

static bool g_running = false;

static std::mutex g_stop_mutex;

using json = nlohmann::json;

static std::vector<std::string> split(const std::string &s, char delimiter) {
  std::vector<std::string> tokens;
  std::string token;
  std::istringstream tokenStream(s);
  while (std::getline(tokenStream, token, delimiter)) {
    tokens.push_back(token);
  }
  return tokens;
}

static void dump_module(std::ofstream& ofs,
                        const std::string& name,
                        std::unordered_map<std::string, std::unordered_set<std::string>>& hierarchy,
                        std::unordered_map<std::string, tap_t*>& tails,
                        int indentation) {
  std::string indent(indentation, ' ');
  ofs << indent << "$scope module " << name << " $end" << std::endl;

  auto itt = tails.find(name);
  if (itt != tails.end()) {
    for (auto& signal : itt->second->signals) {
      ofs << indent << " $var wire " << signal.width << " " << signal.id << " " << signal.name << " $end" << std::endl;
    }
  }

  auto ith = hierarchy.find(name);
  if (ith != hierarchy.end()) {
    for (auto& child : ith->second) {
      dump_module(ofs, child, hierarchy, tails, indentation + 1);
    }
  }

  ofs << indent << "$upscope $end" << std::endl;
}

static void dump_header(std::ofstream& ofs, std::vector<tap_t>& taps) {
  ofs << "$version Generated by Vortex Scope Analyzer $end" << std::endl;
  ofs << "$timescale 1 ns $end" << std::endl;
  ofs << "$scope module TOP $end" << std::endl;
  ofs << " $var wire 1 0 clk $end" << std::endl;

  std::unordered_map<std::string, std::unordered_set<std::string>> hierarchy;
  std::unordered_set<std::string> heads;
  std::unordered_map<std::string, tap_t*> tails;

  // Build hierarchy
  for (auto& tap : taps) {
    std::vector<std::string> tokens = split(tap.path, '.');
    for (size_t i = 1; i < tokens.size(); ++i) {
      hierarchy[tokens[i-1]].insert(tokens[i]);
    }
    auto h = tokens[0];
    auto t = tokens[tokens.size()-1];
    heads.insert(h);
    tails[t] = &tap;
  }

  // Dump module hierarchy
  for (auto& head : heads) {
    dump_module(ofs, head, hierarchy, tails, 1);
  }

  ofs << "$upscope $end" << std::endl;
  ofs << "enddefinitions $end" << std::endl;
}

// return the earliest tap that has data to dump
static tap_t* find_earliest_tap(std::vector<tap_t>& taps) {
  tap_t* earliest = nullptr;
  for (auto& tap : taps) {
    if (tap.samples == 0)
      continue; // skip empty taps
    if (tap.cur_sample == tap.samples)
      continue; // skip finished taps
    if (earliest != nullptr) {
      if (tap.cycle_time < earliest->cycle_time)
        earliest = &tap;
    } else {
      earliest = &tap;
    }
  }
  return earliest;
}

static uint64_t advance_clock(std::ofstream& ofs, uint64_t cur_time, uint64_t next_time) {
  uint64_t delta = next_time - cur_time;
  if (delta > MAX_DELAY_CYCLES) {
    ofs << '#' << (cur_time * 2 + 0) << std::endl;
    ofs << "bx 0" << std::endl;
    ofs << '#' << (cur_time * 2 + 1) << std::endl;
    ofs << "bx 0" << std::endl;
    cur_time = next_time - MAX_DELAY_CYCLES;
  }
  while (cur_time < next_time) {
    ofs << '#' << (cur_time * 2 + 0) << std::endl;
    ofs << "b0 0" << std::endl;
    ofs << '#' << (cur_time * 2 + 1) << std::endl;
    ofs << "b1 0" << std::endl;
    ++cur_time;
  }
  return cur_time;
}

static int dump_tap(std::ofstream& ofs, tap_t* tap, vx_device_h hdevice) {
  uint32_t signal_offset = 0;
  uint32_t sample_offset = 0;
  uint64_t word;

  std::vector<char> signal_data(tap->width);
  auto signal_it = tap->signals.rbegin();
  uint32_t signal_width = signal_it->width;

  do {
    // read data
    uint64_t cmd_data = (tap->id << 3) | CMD_GET_DATA;
    CHECK_ERR(g_callback.registerWrite(hdevice, cmd_data));
    CHECK_ERR(g_callback.registerRead(hdevice, &word));
    do {
      uint32_t word_offset = sample_offset % 64;
      signal_data[signal_width - signal_offset - 1] = ((word >> word_offset) & 0x1) ? '1' : '0';
      ++signal_offset;
      ++sample_offset;
      if (signal_offset == signal_width) {
        signal_data[signal_width] = 0; // string null termination
        ofs << 'b' << signal_data.data() << ' ' << signal_it->id << std::endl;
        if (sample_offset == tap->width) {
          // end-of-sample
          ++tap->cur_sample;
          if (tap->cur_sample != tap->samples) {
            // read next delta
            CHECK_ERR(g_callback.registerWrite(hdevice, cmd_data));
            CHECK_ERR(g_callback.registerRead(hdevice, &word));
            tap->cycle_time += 1 + word;
            if (0 == (tap->cur_sample % SAMPLE_FLUSH_SIZE)) {
              ofs << std::flush;
              std::cout << std::dec << "[SCOPE] flush tap #" << tap->id << ": "<< tap->cur_sample << "/" << tap->samples << " samples, next_time=" << tap->cycle_time << std::endl;
            }
          }
          break;
        }
        signal_offset = 0;
        ++signal_it;
        signal_width = signal_it->width;
      }
    } while ((sample_offset % 64) != 0);
  } while (sample_offset != tap->width);

  return 0;
}

int vx_scope_start(scope_callback_t* callback, vx_device_h hdevice, uint64_t start_time, uint64_t stop_time) {
  if (nullptr == hdevice || nullptr == callback)
    return -1;

  const char* json_path = getenv("SCOPE_JSON_PATH");
  std::ifstream ifs(json_path);
  if (!ifs) {
    std::cerr << "[SCOPE] error: cannot open scope manifest file: " << json_path << std::endl;
    return -1;
  }
  auto json_obj = json::parse(ifs);
  if (json_obj.is_null()) {
    std::cerr << "[SCOPE] error: invalid scope manifest file: " << json_path << std::endl;
    return -1;
  }

  g_callback = *callback;

  // validate scope manifest
  for (auto& tap : json_obj["taps"]) {
    auto id = tap["id"].get<uint32_t>();
    auto width = tap["width"].get<uint32_t>();

    uint64_t cmd_width = (id << 3) | CMD_GET_WIDTH;
    CHECK_ERR(g_callback.registerWrite(hdevice, cmd_width));
    uint64_t dev_width;
    CHECK_ERR(g_callback.registerRead(hdevice, &dev_width));
    if (width != dev_width) {
      std::cerr << "[SCOPE] error: invalid tap #" << id << " width, actual=" << dev_width << ", expected=" << width << std::endl;
      return 1;
    }
  }

  // setup capture size
  const char* capture_size_env = std::getenv("SCOPE_DEPTH");
  if (capture_size_env != nullptr) {
    std::stringstream ss(capture_size_env);
    uint32_t capture_size;
    if (ss >> capture_size) {
      for (auto& tap : json_obj["taps"]) {
        auto id = tap["id"].get<uint32_t>();
        uint64_t cmd_depth = (capture_size << 11) | (id << 3) | CMD_SET_DEPTH;
        CHECK_ERR(g_callback.registerWrite(hdevice, cmd_depth));
      }
    }
  }

  // set stop time
  if (stop_time != uint64_t(-1)) {
    std::cout << "[SCOPE] stop time: " << std::dec << stop_time << "s" << std::endl;
    for (auto& tap : json_obj["taps"]) {
      auto id = tap["id"].get<uint32_t>();
      uint64_t cmd_stop = (stop_time << 11) | (id << 3) | CMD_SET_STOP;
      CHECK_ERR(g_callback.registerWrite(hdevice, cmd_stop));
    }
  }

  // start recording
  if (start_time != uint64_t(-1)) {
    std::cout << "[SCOPE] start time: " << std::dec << start_time << "s" << std::endl;
    for (auto& tap : json_obj["taps"]) {
      auto id = tap["id"].get<uint32_t>();
      uint64_t cmd_start = (start_time << 11) | (id << 3) | CMD_SET_START;
      CHECK_ERR(g_callback.registerWrite(hdevice, cmd_start));
    }
  }

  g_running = true;

  // create auto-stop thread
  uint32_t timeout_time = TIMEOUT_TIME;
  const char* env_timeout = std::getenv("SCOPE_TIMEOUT");
  if (env_timeout != nullptr) {
    std::stringstream ss(env_timeout);
    uint32_t env_value;
    if (ss >> env_value) {
      timeout_time = env_value;
      std::cout << "[SCOPE] timeout time=" << env_value << std::endl;
    }
  }
  std::thread([hdevice, timeout_time]() {
    std::this_thread::sleep_for(std::chrono::seconds(timeout_time));
    std::cout << "[SCOPE] auto-stop timeout!" << std::endl;
    vx_scope_stop(hdevice);
  }).detach();

  return 0;
}

int vx_scope_stop(vx_device_h hdevice) {
  std::lock_guard<std::mutex> lock(g_stop_mutex);

  if (nullptr == hdevice)
    return -1;

  if (!g_running)
    return 0;

  g_running = false;

  std::vector<tap_t> taps;

  {
    const char* json_path = getenv("SCOPE_JSON_PATH");
    std::ifstream ifs(json_path);
    auto json_obj = json::parse(ifs);
    if (json_obj.is_null())
      return 0;

    uint32_t signal_id = 1;

    for (auto& tap : json_obj["taps"]) {
      tap_t _tap;
      _tap.id    = tap["id"].get<uint32_t>();
      _tap.width = tap["width"].get<uint32_t>();
      _tap.path  = tap["path"].get<std::string>();
      _tap.cycle_time = 0;
      _tap.samples = 0;
      _tap.cur_sample = 0;

      for (auto& signal : tap["signals"]) {
        auto name  = signal[0].get<std::string>();
        auto width = signal[1].get<uint32_t>();
        _tap.signals.push_back({signal_id, name, width});
        ++signal_id;
      }

      taps.emplace_back(std::move(_tap));
    }
  }

  std::cout << "[SCOPE] stop recording..." << std::endl;

  for (auto& tap : taps) {
    uint64_t cmd_stop = (0 << 11) | (tap.id << 3) | CMD_SET_STOP;
    CHECK_ERR(g_callback.registerWrite(hdevice, cmd_stop));
  }

  std::cout << "[SCOPE] load trace info..." << std::endl;

  for (auto& tap : taps) {
    uint64_t count, start, delta;

    // get count
    uint64_t cmd_count = (tap.id << 3) | CMD_GET_COUNT;
    CHECK_ERR(g_callback.registerWrite(hdevice, cmd_count));
    CHECK_ERR(g_callback.registerRead(hdevice, &count));
    if (count == 0)
      continue;

    // get start
    uint64_t cmd_start = (tap.id << 3) | CMD_GET_START;
    CHECK_ERR(g_callback.registerWrite(hdevice, cmd_start));
    CHECK_ERR(g_callback.registerRead(hdevice, &start));

    // get delta
    uint64_t cmd_data = (tap.id << 3) | CMD_GET_DATA;
    CHECK_ERR(g_callback.registerWrite(hdevice, cmd_data));
    CHECK_ERR(g_callback.registerRead(hdevice, &delta));

    tap.samples = count;
    tap.cycle_time = 1 + start + delta;

    std::cout << std::dec << "[SCOPE] tap #" << tap.id
              << ": width=" << tap.width
              << ", num_samples=" << tap.samples
              << ", start_time=" << tap.cycle_time
              << ", path=" << tap.path << std::endl;
  }

  std::cout << "[SCOPE] dump header..." << std::endl;

  std::ofstream ofs("scope.vcd");

  dump_header(ofs, taps);

  std::cout << "[SCOPE] dump taps..." << std::endl;

  uint64_t cur_time = 0;
  auto tap = find_earliest_tap(taps);
  if (tap != nullptr) {
    do {
      // advance clock
      cur_time = advance_clock(ofs, cur_time, tap->cycle_time);
      // dump tap
      CHECK_ERR(dump_tap(ofs, tap, hdevice));
      // find the nearest tap
      tap = find_earliest_tap(taps);
    } while (tap != nullptr);
    // advance clock
    advance_clock(ofs, cur_time, cur_time + 1);
  }

  std::cout << "[SCOPE] trace dump done! - " << (cur_time/2) << " cycles" << std::endl;

  return 0;
}
