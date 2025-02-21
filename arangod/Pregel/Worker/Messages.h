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
/// @author Julia Volmer
////////////////////////////////////////////////////////////////////////////////
#pragma once

#include "Actor/ActorPID.h"
#include "Cluster/ClusterTypes.h"
#include "Inspection/Format.h"
#include "Inspection/Types.h"
#include "Pregel/CollectionSpecifications.h"
#include "Pregel/Conductor/Messages.h"
#include "Pregel/ExecutionNumber.h"
#include "Pregel/GraphStore/Graph.h"
#include "Pregel/PregelOptions.h"
#include "Pregel/Statistics.h"
#include "Pregel/Status/Status.h"

namespace arangodb::pregel {

namespace worker::message {

struct CreateNewWorker {
  ExecutionSpecifications executionSpecifications;
  CollectionSpecifications collectionSpecifications;
};
template<typename Inspector>
auto inspect(Inspector& f, CreateNewWorker& x) {
  return f.object(x).fields(
      f.field("executionSpecifications", x.executionSpecifications),
      f.field("collectionSpecifications", x.collectionSpecifications));
}

struct WorkerStart {};
template<typename Inspector>
auto inspect(Inspector& f, WorkerStart& x) {
  return f.object(x).fields();
}

struct WorkerMessages : std::variant<WorkerStart, CreateNewWorker> {
  using std::variant<WorkerStart, CreateNewWorker>::variant;
};
template<typename Inspector>
auto inspect(Inspector& f, WorkerMessages& x) {
  return f.variant(x).unqualified().alternatives(
      arangodb::inspection::type<WorkerStart>("Start"),
      arangodb::inspection::type<CreateNewWorker>("CreateWorker"));
}

}  // namespace worker::message

struct GraphLoaded {
  ExecutionNumber executionNumber;
  std::string sender;
  uint64_t vertexCount;
  uint64_t edgeCount;
};

template<typename Inspector>
auto inspect(Inspector& f, GraphLoaded& x) {
  return f.object(x).fields(
      f.field(Utils::executionNumberKey, x.executionNumber),
      f.field("sender", x.sender), f.field("vertexCount", x.vertexCount),
      f.field("edgeCount", x.edgeCount));
}

struct GlobalSuperStepPrepared {
  ExecutionNumber executionNumber;
  std::string sender;
  uint64_t activeCount;
  uint64_t vertexCount;
  uint64_t edgeCount;
  VPackBuilder aggregators;
};
template<typename Inspector>
auto inspect(Inspector& f, GlobalSuperStepPrepared& x) {
  return f.object(x).fields(
      f.field(Utils::executionNumberKey, x.executionNumber),
      f.field("sender", x.sender), f.field("activeCount", x.activeCount),
      f.field("vertexCount", x.vertexCount), f.field("edgeCount", x.edgeCount),
      f.field("aggregators", x.aggregators));
}

struct GlobalSuperStepFinished {
  ExecutionNumber executionNumber;
  std::string sender;
  uint64_t gss;
  MessageStats messageStats;
};

template<typename Inspector>
auto inspect(Inspector& f, GlobalSuperStepFinished& x) {
  return f.object(x).fields(
      f.field(Utils::executionNumberKey, x.executionNumber),
      f.field("sender", x.sender), f.field("gss", x.gss),
      f.field("messageStats", x.messageStats));
}

struct Finished {
  ExecutionNumber executionNumber;
  std::string sender;
};
template<typename Inspector>
auto inspect(Inspector& f, Finished& x) {
  return f.object(x).fields(
      f.field(Utils::executionNumberKey, x.executionNumber),
      f.field("sender", x.sender));
}

struct StatusUpdated {
  ExecutionNumber executionNumber;
  std::string sender;
  Status status;
};
template<typename Inspector>
auto inspect(Inspector& f, StatusUpdated& x) {
  return f.object(x).fields(
      f.field(Utils::executionNumberKey, x.executionNumber),
      f.field("sender", x.sender), f.field("status", x.status));
}

struct PregelResults {
  VPackBuilder results;
};
template<typename Inspector>
auto inspect(Inspector& f, PregelResults& x) {
  return f.object(x).fields(f.field("results", x.results));
}

struct PregelMessage {
  ExecutionNumber executionNumber;
  uint64_t gss;
  PregelShard shard;
  VPackBuilder messages;
};

template<typename Inspector>
auto inspect(Inspector& f, PregelMessage& x) {
  return f.object(x).fields(
      f.field(Utils::executionNumberKey, x.executionNumber),
      f.field(Utils::globalSuperstepKey, x.gss), f.field("shard", x.shard),
      f.field("messages", x.messages));
}

}  // namespace arangodb::pregel

template<>
struct fmt::formatter<arangodb::pregel::StatusUpdated>
    : arangodb::inspection::inspection_formatter {};
template<>
struct fmt::formatter<arangodb::pregel::GlobalSuperStepFinished>
    : arangodb::inspection::inspection_formatter {};
template<>
struct fmt::formatter<arangodb::pregel::worker::message::CreateNewWorker>
    : arangodb::inspection::inspection_formatter {};
