/*
 * Copyright 2011-2021 Blender Foundation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include "device/device_queue.h"
#include "integrator/path_trace_work.h"
#include "util/util_vector.h"

CCL_NAMESPACE_BEGIN

struct KernelWorkTile;

/* Implementation of PathTraceWork which schedules work on to queues pixel-by-pixel.
 * This implementation suits best for the CPU device.
 *
 * NOTE: For the CPU rendering there are assumptions about TBB arena size and number of concurrent
 * queues on the render device which makes this work be only usable on CPU. */
class PathTraceWorkPixel : public PathTraceWork {
 public:
  PathTraceWorkPixel(Device *render_device, RenderBuffers *buffers, bool *cancel_requested_flag);

  virtual void init_execution() override;

  virtual void render_samples(const BufferParams &scaled_render_buffer_params,
                              int start_sample,
                              int samples_num) override;

 protected:
  /* This is a worker thread's "run" function which polls for a work to be rendered and renders
   * the work. */
  void render_samples_full_pipeline(DeviceQueue *queue);

  /* Core path tracing routine. Renders given work time on the given queue. */
  void render_samples_full_pipeline(DeviceQueue *queue,
                                    const KernelWorkTile &work_tile,
                                    const int samples_num);

  /* Integrator queues.
   * There are as many of queues as the concurrent queues the device supports. */
  vector<unique_ptr<DeviceQueue>> integrator_queues_;

  /* Use queue which corresponds to a current thread index within the arena.
   * Used for CPU rendering where threads need to have a way to know which queue to use. */
  bool use_thread_index_queue_ = false;
};

CCL_NAMESPACE_END