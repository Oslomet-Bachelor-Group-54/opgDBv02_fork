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
/// @author Simon Grätzer
////////////////////////////////////////////////////////////////////////////////

#include "SCC.h"
#include <atomic>
#include <climits>
#include <utility>
#include "Cluster/ClusterInfo.h"
#include "Cluster/ServerState.h"
#include "Pregel/Aggregator.h"
#include "Pregel/Algorithm.h"
#include "Pregel/Worker/GraphStore.h"
#include "Pregel/IncomingCache.h"
#include "Pregel/MasterContext.h"
#include "Pregel/VertexComputation.h"
#include "Pregel/SenderMessage.h"
#include "Pregel/SenderMessageFormat.h"

using namespace arangodb;
using namespace arangodb::pregel;
using namespace arangodb::pregel::algos;

namespace {
static std::string const kPhase = "phase";
static std::string const kFoundNewMax = "max";
static std::string const kConverged = "converged";

enum SCCPhase {
  TRANSPOSE = 0,
  TRIMMING = 1,
  FORWARD_TRAVERSAL = 2,
  BACKWARD_TRAVERSAL_START = 3,
  BACKWARD_TRAVERSAL_REST = 4
};

struct SCCComputation
    : public VertexComputation<SCCValue, int8_t, SenderMessage<uint64_t>> {
  SCCComputation() = default;

  void compute(
      MessageIterator<SenderMessage<uint64_t>> const& messages) override {
    if (not isActive()) {
      // color was already determined or vertex was trimmed
      return;
    }

    SCCValue* vertexState = mutableVertexData();
    auto const& phase = getAggregatedValueRef<uint32_t>(kPhase);

    switch (phase) {
      // let all our connected nodes know we are there
      case SCCPhase::TRANSPOSE: {
        vertexState->parents.clear();
        SenderMessage<uint64_t> message(pregelId(), 0);
        sendMessageToAllNeighbours(message);
        break;
      }

      // Creates list of parents based on the received ids and halts the
      // vertices
      // that don't have any parent or outgoing edge, hence, they can't be
      // part of an SCC.
      case SCCPhase::TRIMMING: {
        for (SenderMessage<uint64_t> const* msg : messages) {
          vertexState->parents.push_back(msg->senderId);
        }
        // reset the color to the vertex ID
        vertexState->color = vertexState->vertexID;
        // If this node doesn't have any parents or outgoing edges,
        // it can't be part of an SCC
        if (vertexState->parents.empty() || getEdgeCount() == 0) {
          voteHalt();  // this makes the vertex inactive
        } else {
          SenderMessage<uint64_t> message(pregelId(), vertexState->color);
          sendMessageToAllNeighbours(message);
        }
        break;
      }

      /// Traverse the graph through outgoing edges and keep the maximum vertex
      /// value. If a new maximum value is found, propagate it until
      /// convergence.
      case SCCPhase::FORWARD_TRAVERSAL: {
        uint64_t oldColor = vertexState->color;
        for (SenderMessage<uint64_t> const* msg : messages) {
          if (vertexState->color < msg->value) {
            vertexState->color = msg->value;
          }
        }
        if (oldColor != vertexState->color) {
          SenderMessage<uint64_t> message(pregelId(), vertexState->color);
          sendMessageToAllNeighbours(message);
          aggregate(kFoundNewMax, true);
        }
        break;
      }

      /// Traverse the transposed graph and keep the maximum vertex value.
      case SCCPhase::BACKWARD_TRAVERSAL_START: {
        // if my color was not overwritten, start backwards traversal
        if (vertexState->vertexID == vertexState->color) {
          SenderMessage<uint64_t> message(pregelId(), vertexState->color);
          // sendMessageToAllParents
          for (VertexID const& pid : vertexState->parents) {
            sendMessage(pid, message);  // todo: if the parent was deactivated
                                        //  this reactivates it in the
                                        //  refactored Pregel. Change this.
          }
        }
        break;
      }

      /// Traverse the transposed graph and keep the maximum vertex value.
      case SCCPhase::BACKWARD_TRAVERSAL_REST: {
        for (SenderMessage<uint64_t> const* msg : messages) {
          if (vertexState->color == msg->value) {
            for (VertexID const& pid : vertexState->parents) {
              sendMessage(pid, *msg);
            }
            aggregate(kConverged, true);
            voteHalt();
            break;
          }
        }
        break;
      }
    }
  }
};

}  // namespace

VertexComputation<SCCValue, int8_t, SenderMessage<uint64_t>>*
SCC::createComputation(WorkerConfig const* config) const {
  return new SCCComputation();
}

namespace {

struct SCCGraphFormat : public GraphFormat<SCCValue, int8_t> {
  const std::string _resultField;

  explicit SCCGraphFormat(application_features::ApplicationServer& server,
                          std::string result)
      : GraphFormat<SCCValue, int8_t>(server),
        _resultField(std::move(result)) {}

  [[nodiscard]] size_t estimatedEdgeSize() const override { return 0; }

  void copyVertexData(arangodb::velocypack::Options const&,
                      std::string const& documentId,
                      arangodb::velocypack::Slice document, SCCValue& senders,
                      uint64_t& vertexIdRange) override {
    senders.vertexID = vertexIdRange++;
  }

  bool buildVertexDocument(arangodb::velocypack::Builder& b,
                           SCCValue const* ptr) const override {
    if (ptr->color != INT_MAX) {
      b.add(_resultField, VPackValue(ptr->color));
    } else {
      b.add(_resultField, VPackValue(-1));
    }
    return true;
  }
};

}  // namespace

GraphFormat<SCCValue, int8_t>* SCC::inputFormat() const {
  return new SCCGraphFormat(_server, _resultField);
}

struct SCCMasterContext : public MasterContext {
  SCCMasterContext() = default;  // TODO use _threshold
  void preGlobalSuperstep() override {
    if (globalSuperstep() == 0) {
      aggregate<uint32_t>(kPhase, SCCPhase::TRANSPOSE);
      return;
    }

    auto const* phase = getAggregatedValue<uint32_t>(kPhase);
    switch (*phase) {
      case SCCPhase::TRANSPOSE:
        LOG_TOPIC("d9208", DEBUG, Logger::PREGEL) << "Phase: TRANSPOSE";
        aggregate<uint32_t>(kPhase, SCCPhase::TRIMMING);
        break;

      case SCCPhase::TRIMMING:
        LOG_TOPIC("9dec9", DEBUG, Logger::PREGEL) << "Phase: TRIMMING";
        aggregate<uint32_t>(kPhase, SCCPhase::FORWARD_TRAVERSAL);
        break;

      case SCCPhase::FORWARD_TRAVERSAL: {
        LOG_TOPIC("4d39d", DEBUG, Logger::PREGEL) << "Phase: FORWARD_TRAVERSAL";
        bool const* newMaxFound = getAggregatedValue<bool>(kFoundNewMax);
        if (not *newMaxFound) {
          aggregate<uint32_t>(kPhase, SCCPhase::BACKWARD_TRAVERSAL_START);
        }
      } break;

      case SCCPhase::BACKWARD_TRAVERSAL_START:
        LOG_TOPIC("fc62a", DEBUG, Logger::PREGEL)
            << "Phase: BACKWARD_TRAVERSAL_START";
        aggregate<uint32_t>(kPhase, SCCPhase::BACKWARD_TRAVERSAL_REST);
        break;

      case SCCPhase::BACKWARD_TRAVERSAL_REST:
        LOG_TOPIC("905b0", DEBUG, Logger::PREGEL)
            << "Phase: BACKWARD_TRAVERSAL_REST";
        bool const* converged = getAggregatedValue<bool>(kConverged);
        // continue until no more vertices are updated
        if (not *converged) {
          aggregate<uint32_t>(kPhase, SCCPhase::TRANSPOSE);
        }
        break;
    }
  };
};

MasterContext* SCC::masterContext(VPackSlice userParams) const {
  return new SCCMasterContext();
}

IAggregator* SCC::aggregator(std::string const& name) const {
  if (name == kPhase) {  // permanent value
    return new OverwriteAggregator<uint32_t>(SCCPhase::TRANSPOSE, true);
  } else if (name == kFoundNewMax) {
    return new BoolOrAggregator(false);  // non perm
  } else if (name == kConverged) {
    return new BoolOrAggregator(false);  // non perm
  }
  return nullptr;
}
