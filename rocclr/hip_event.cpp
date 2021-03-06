/* Copyright (c) 2015-present Advanced Micro Devices, Inc.

 Permission is hereby granted, free of charge, to any person obtaining a copy
 of this software and associated documentation files (the "Software"), to deal
 in the Software without restriction, including without limitation the rights
 to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 copies of the Software, and to permit persons to whom the Software is
 furnished to do so, subject to the following conditions:

 The above copyright notice and this permission notice shall be included in
 all copies or substantial portions of the Software.

 THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 THE SOFTWARE. */

#include <hip/hip_runtime.h>

#include "hip_event.hpp"

namespace hip {

bool Event::ready() {
  if (event_->status() != CL_COMPLETE) {
    event_->notifyCmdQueue();
  }

  return (event_->status() == CL_COMPLETE);
}

hipError_t Event::query() {
  amd::ScopedLock lock(lock_);

  // If event is not recorded, event_ is null, hence return hipSuccess
  if (event_ == nullptr) {
    return hipSuccess;
  }

  return ready() ? hipSuccess : hipErrorNotReady;
}

hipError_t Event::synchronize() {
  amd::ScopedLock lock(lock_);

  // If event is not recorded, event_ is null, hence return hipSuccess
  if (event_ == nullptr) {
    return hipSuccess;
  }

  event_->awaitCompletion();

  return hipSuccess;
}

hipError_t Event::elapsedTime(Event& eStop, float& ms) {
  amd::ScopedLock startLock(lock_);

  if (this == &eStop) {
    if (event_ == nullptr) {
      return hipErrorInvalidHandle;
    }

    if (flags & hipEventDisableTiming) {
      return hipErrorInvalidHandle;
    }

    if (!ready()) {
      return hipErrorNotReady;
    }

    ms = 0.f;
    return hipSuccess;
  }
  amd::ScopedLock stopLock(eStop.lock_);

  if (event_ == nullptr ||
      eStop.event_  == nullptr) {
    return hipErrorInvalidHandle;
  }

  if ((flags | eStop.flags) & hipEventDisableTiming) {
    return hipErrorInvalidHandle;
  }

  if (!ready() || !eStop.ready()) {
    return hipErrorNotReady;
  }

  if (event_ != eStop.event_ && recorded_ && eStop.recorded_) {
    ms = static_cast<float>(static_cast<int64_t>(eStop.event_->profilingInfo().end_ -
                          event_->profilingInfo().end_))/1000000.f;
  } else if (event_ == eStop.event_ && (recorded_ || eStop.recorded_)) {
    // Events are the same, which indicates the stream is empty and likely
    // eventRecord is called on another stream. For such cases insert and measure a
    // marker.
    amd::Command* command = new amd::Marker(*event_->command().queue(), kMarkerDisableFlush);
    command->enqueue();
    command->awaitCompletion();
    ms = static_cast<float>(static_cast<int64_t>(command->event().profilingInfo().end_ -
                          event_->profilingInfo().end_))/1000000.f;
    command->release();

  } else {
  // For certain HIP API's that take both start and stop event
  // or scenarios where HIP API takes one of the events and the other event is recorded with hipEventRecord
    ms = static_cast<float>(static_cast<int64_t>(eStop.event_->profilingInfo().end_ -
                          event_->profilingInfo().start_))/1000000.f;
  }
  return hipSuccess;
}

hipError_t Event::streamWait(amd::HostQueue* hostQueue, uint flags) {
  if ((event_ == nullptr) || (event_->command().queue() == hostQueue)) {
    return hipSuccess;
  }

  amd::ScopedLock lock(lock_);
  bool retain = false;

  if (!event_->notifyCmdQueue()) {
    return hipErrorLaunchOutOfResources;
  }
  amd::Command::EventWaitList eventWaitList;
  eventWaitList.push_back(event_);

  amd::Command* command = new amd::Marker(*hostQueue, kMarkerDisableFlush, eventWaitList);
  if (command == NULL) {
    return hipErrorOutOfMemory;
  }
  command->enqueue();
  command->release();

  return hipSuccess;
}

void Event::addMarker(amd::HostQueue* queue, amd::Command* command, bool record) {
  amd::ScopedLock lock(lock_);

  if (queue->properties().test(CL_QUEUE_PROFILING_ENABLE)) {
    if (command == nullptr) {
      command = queue->getLastQueuedCommand(true);
      if ((command == nullptr) || (command->type() == 0)) {
        // if lastEnqueuedCommand is user invisible command(command->type() == 0),
        // which is only used for sync, create a new amd:Marker
        // and release() lastEnqueuedCommand
        if (command != nullptr) {
          command->release();
        }
        command = new amd::Marker(*queue, kMarkerDisableFlush);
        command->enqueue();
      }
    }
  } else if (command == nullptr) {
    command = new hip::ProfileMarker(*queue, false);
    command->enqueue();
  }

  if (event_ == &command->event()) return;

  if (event_ != nullptr) {
    event_->release();
  }

  event_ = &command->event();
  recorded_ = record;
}

}

hipError_t ihipEventCreateWithFlags(hipEvent_t* event, unsigned flags) {
  if (event == nullptr) {
    return hipErrorInvalidValue;
  }

  unsigned supportedFlags = hipEventDefault | hipEventBlockingSync | hipEventDisableTiming |
                            hipEventReleaseToDevice | hipEventReleaseToSystem;
  const unsigned releaseFlags = (hipEventReleaseToDevice | hipEventReleaseToSystem);

  const bool illegalFlags =
      (flags & ~supportedFlags) ||             // can't set any unsupported flags.
      (flags & releaseFlags) == releaseFlags;  // can't set both release flags

  if (!illegalFlags) {
    hip::Event* e = new hip::Event(flags);

    if (e == nullptr) {
      return hipErrorOutOfMemory;
    }

    *event = reinterpret_cast<hipEvent_t>(e);
  } else {
    return hipErrorInvalidValue;
  }
  return hipSuccess;
}

hipError_t ihipEventQuery(hipEvent_t event) {
  if (event == nullptr) {
    return hipErrorInvalidHandle;
  }

  hip::Event* e = reinterpret_cast<hip::Event*>(event);

  return e->query();
}

hipError_t hipEventCreateWithFlags(hipEvent_t* event, unsigned flags) {
  HIP_INIT_API(hipEventCreateWithFlags, event, flags);

  HIP_RETURN(ihipEventCreateWithFlags(event, flags), *event);
}

hipError_t hipEventCreate(hipEvent_t* event) {
  HIP_INIT_API(hipEventCreate, event);

  HIP_RETURN(ihipEventCreateWithFlags(event, 0), *event);
}

hipError_t hipEventDestroy(hipEvent_t event) {
  HIP_INIT_API(hipEventDestroy, event);

  if (event == nullptr) {
    HIP_RETURN(hipErrorInvalidHandle);
  }

  delete reinterpret_cast<hip::Event*>(event);

  HIP_RETURN(hipSuccess);
}

hipError_t hipEventElapsedTime(float *ms, hipEvent_t start, hipEvent_t stop) {
  HIP_INIT_API(hipEventElapsedTime, ms, start, stop);

  if (start == nullptr || stop == nullptr) {
    HIP_RETURN(hipErrorInvalidHandle);
  }

  if (ms == nullptr) {
    HIP_RETURN(hipErrorInvalidValue);
  }

  hip::Event* eStart = reinterpret_cast<hip::Event*>(start);
  hip::Event* eStop  = reinterpret_cast<hip::Event*>(stop);

  HIP_RETURN(eStart->elapsedTime(*eStop, *ms), "Elapsed Time = ", *ms);
}

// ================================================================================================
hipError_t hipEventRecord(hipEvent_t event, hipStream_t stream) {
  HIP_INIT_API(hipEventRecord, event, stream);

  if (event == nullptr) {
    HIP_RETURN(hipErrorInvalidHandle);
  }

  hip::Event* e = reinterpret_cast<hip::Event*>(event);
  amd::HostQueue* queue = hip::getQueue(stream);

  e->addMarker(queue, nullptr, true);
  HIP_RETURN(hipSuccess);
}

// ================================================================================================
hipError_t hipEventSynchronize(hipEvent_t event) {
  HIP_INIT_API(hipEventSynchronize, event);

  if (event == nullptr) {
    HIP_RETURN(hipErrorInvalidHandle);
  }

  hip::Event* e = reinterpret_cast<hip::Event*>(event);

  HIP_RETURN(e->synchronize());
}

hipError_t hipEventQuery(hipEvent_t event) {
  HIP_INIT_API(hipEventQuery, event);

  HIP_RETURN(ihipEventQuery(event));
}
