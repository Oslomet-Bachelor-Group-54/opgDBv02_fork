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
/// @author Jan Steemann
////////////////////////////////////////////////////////////////////////////////

#pragma once

#include "Basics/Common.h"
#include "VocBase/voc-types.h"

struct TRI_vocbase_t;

namespace arangodb {
class DatabaseFeature;

struct IDatabaseGuard {
  virtual ~IDatabaseGuard() = default;
  [[nodiscard]] virtual TRI_vocbase_t& database() const noexcept = 0;
};

struct VocbaseReleaser {
  void operator()(TRI_vocbase_t* vocbase) const noexcept;
};

using VocbasePtr = std::unique_ptr<TRI_vocbase_t, VocbaseReleaser>;

/// @brief Scope guard for a database, ensures that it is not
///        dropped while still using it.
class DatabaseGuard final : public IDatabaseGuard {
 public:
  /// @brief create guard on existing db
  explicit DatabaseGuard(TRI_vocbase_t& vocbase);

  /// @brief create the guard, using a database id
  DatabaseGuard(DatabaseFeature& feature, TRI_voc_tick_t id);

  /// @brief create the guard, using a database name
  DatabaseGuard(DatabaseFeature& feature, std::string_view name);

  /// @brief return the database pointer
  TRI_vocbase_t& database() const noexcept final { return *_vocbase; }
  TRI_vocbase_t const* operator->() const noexcept { return _vocbase.get(); }
  TRI_vocbase_t* operator->() noexcept { return _vocbase.get(); }

 private:
  explicit DatabaseGuard(VocbasePtr vocbase);

  VocbasePtr _vocbase;
};

}  // namespace arangodb
