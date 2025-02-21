////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2023 ArangoDB GmbH, Cologne, Germany
/// Copyright 2004-2014 triAGENS GmbH, Cologne, Germany
///
/// Licensed under the Apache License, Version 2.0 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     http://www.apache.org/licenses/LICENSE-2.0
///
/// Unless required by applicable law or agreed to in writing, software
/// distributed under the License is distributed on an "AS IS" BASIS,
/// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
/// See the License for the specific language governing permissions and
/// limitations under the License.
///
/// Copyright holder is ArangoDB GmbH, Cologne, Germany
///
/// @author Dan Larkin-York
////////////////////////////////////////////////////////////////////////////////

#include "Cache/ManagerTasks.h"
#include "Basics/SpinLocker.h"
#include "Cache/Cache.h"
#include "Cache/Manager.h"
#include "Cache/Metadata.h"

namespace arangodb::cache {

FreeMemoryTask::FreeMemoryTask(Manager::TaskEnvironment environment,
                               Manager& manager, std::shared_ptr<Cache> cache)
    : _environment(environment), _manager(manager), _cache(std::move(cache)) {}

FreeMemoryTask::~FreeMemoryTask() = default;

bool FreeMemoryTask::dispatch() {
  _manager.prepareTask(_environment);
  try {
    if (_manager.post([self = shared_from_this()]() -> void { self->run(); })) {
      return true;
    }
    _manager.unprepareTask(_environment);
    return false;
  } catch (std::exception const&) {
    _manager.unprepareTask(_environment);
    throw;
  }
}

void FreeMemoryTask::run() {
  try {
    using basics::SpinLocker;

    bool ran = _cache->freeMemory();

    if (ran) {
      std::uint64_t reclaimed = 0;
      SpinLocker guard(SpinLocker::Mode::Write, _manager._lock);
      Metadata& metadata = _cache->metadata();
      {
        SpinLocker metaGuard(SpinLocker::Mode::Write, metadata.lock());
        TRI_ASSERT(metaGuard.isLocked());
        reclaimed = metadata.hardUsageLimit - metadata.softUsageLimit;
        metadata.adjustLimits(metadata.softUsageLimit, metadata.softUsageLimit);
        metadata.toggleResizing();
      }
      _manager._globalAllocation -= reclaimed;
    }

    _manager.unprepareTask(_environment);
  } catch (std::exception const&) {
    // always count down at the end
    _manager.unprepareTask(_environment);
    throw;
  }
}

MigrateTask::MigrateTask(Manager::TaskEnvironment environment, Manager& manager,
                         std::shared_ptr<Cache> cache,
                         std::shared_ptr<Table> table)
    : _environment(environment),
      _manager(manager),
      _cache(std::move(cache)),
      _table(std::move(table)) {}

MigrateTask::~MigrateTask() = default;

bool MigrateTask::dispatch() {
  _manager.prepareTask(_environment);
  try {
    if (_manager.post([self = shared_from_this()]() -> void { self->run(); })) {
      return true;
    }
    _manager.unprepareTask(_environment);
    return false;
  } catch (std::exception const&) {
    _manager.unprepareTask(_environment);
    throw;
  }
}

void MigrateTask::run() {
  try {
    using basics::SpinLocker;

    // do the actual migration
    bool ran = _cache->migrate(_table);

    if (!ran) {
      Metadata& metadata = _cache->metadata();
      {
        SpinLocker metaGuard(SpinLocker::Mode::Write, metadata.lock());
        TRI_ASSERT(metaGuard.isLocked());
        metadata.toggleMigrating();
      }
      _manager.reclaimTable(std::move(_table), false);
      TRI_ASSERT(_table == nullptr);
    }

    _manager.unprepareTask(_environment);
  } catch (std::exception const&) {
    // always count down at the end
    _manager.unprepareTask(_environment);
    throw;
  }
}
}  // namespace arangodb::cache
