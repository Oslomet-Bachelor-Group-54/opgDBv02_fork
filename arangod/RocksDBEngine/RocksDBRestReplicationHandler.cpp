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
/// @author Jan Christoph Uhde
////////////////////////////////////////////////////////////////////////////////

#include "RocksDBRestReplicationHandler.h"

#include "ApplicationFeatures/ApplicationServer.h"
#include "Basics/StaticStrings.h"
#include "Basics/StringBuffer.h"
#include "Basics/VPackStringBufferAdapter.h"
#include "Basics/VelocyPackHelper.h"
#include "Basics/system-functions.h"
#include "Logger/LogMacros.h"
#include "Replication/ReplicationClients.h"
#include "Replication/ReplicationFeature.h"
#include "Replication/Syncer.h"
#include "Replication/utilities.h"
#include "Rest/HttpResponse.h"
#include "RestServer/DatabaseFeature.h"
#include "RocksDBEngine/RocksDBCommon.h"
#include "RocksDBEngine/RocksDBEngine.h"
#include "RocksDBEngine/RocksDBReplicationContext.h"
#include "RocksDBEngine/RocksDBReplicationContextGuard.h"
#include "RocksDBEngine/RocksDBReplicationManager.h"
#include "RocksDBEngine/RocksDBReplicationTailing.h"
#include "StorageEngine/EngineSelectorFeature.h"
#include "StorageEngine/PhysicalCollection.h"
#include "StorageEngine/StorageEngine.h"
#include "Transaction/StandaloneContext.h"
#include "VocBase/LogicalCollection.h"
#include "VocBase/ticks.h"

#include <string_view>

#include <absl/strings/str_cat.h>

#include <velocypack/Builder.h>
#include <velocypack/Dumper.h>
#include <velocypack/Iterator.h>
#include <velocypack/Slice.h>

using namespace arangodb;
using namespace arangodb::basics;
using namespace arangodb::rest;
using namespace arangodb::rocksutils;

RocksDBRestReplicationHandler::RocksDBRestReplicationHandler(
    ArangodServer& server, GeneralRequest* request, GeneralResponse* response)
    : RestReplicationHandler(server, request, response),
      _manager(server.getFeature<EngineSelectorFeature>()
                   .engine<RocksDBEngine>()
                   .replicationManager()),
      _quickKeysNumDocsLimit(
          server.getFeature<ReplicationFeature>().quickKeysLimit()) {
#ifdef ARANGODB_ENABLE_FAILURE_TESTS
  adjustQuickKeysNumDocsLimit();
#endif
}

void RocksDBRestReplicationHandler::handleCommandBatch() {
  // extract the request type
  auto const type = _request->requestType();
  auto const& suffixes = _request->suffixes();
  size_t const len = suffixes.size();

  TRI_ASSERT(len >= 1);

  if (type == rest::RequestType::POST) {
    // create a new blocker

    bool parseSuccess = true;
    VPackSlice body = this->parseVPackBody(parseSuccess);
    if (!parseSuccess || !body.isObject()) {  // error already created
      return;
    }
    std::string patchCount =
        VelocyPackHelper::getStringValue(body, "patchCount", "");

    arangodb::ServerId const clientId{
        StringUtils::uint64(_request->value("serverId"))};
    SyncerId const syncerId = SyncerId::fromRequest(*_request);
    std::string const clientInfo = _request->value("clientInfo");

    // create transaction+snapshot, ttl will be default if `ttl == 0``
    auto ttl = VelocyPackHelper::getNumericValue<double>(
        body, "ttl", replutils::BatchInfo::DefaultTimeout);
    auto& engine =
        server().getFeature<EngineSelectorFeature>().engine<RocksDBEngine>();
    auto ctx =
        _manager->createContext(engine, ttl, syncerId, clientId, patchCount);

    if (!patchCount.empty()) {
      auto triple = ctx->bindCollectionIncremental(_vocbase, patchCount);
      Result res = std::get<0>(triple);
      if (res.fail()) {
        LOG_TOPIC("3d5d4", WARN, Logger::REPLICATION)
            << "Error during first phase of"
            << " collection count patching: " << res.errorMessage();
      }
    }

    std::string const snapTick = std::to_string(ctx->snapshotTick());
    bool withState = _request->parsedValue("state", false);

    VPackBuilder b;
    b.openObject();
    b.add("id", VPackValue(std::to_string(ctx->id())));  // id always string
    b.add("lastTick", VPackValue(snapTick));
    if (withState) {
      // we have been asked to also return the "state" attribute.
      // this is used from 3.8 onwards during shard synchronization, in order
      // to combine the two requests for starting a batch and fetching the
      // leader state into a single one.

      // get original logger state data
      VPackBuilder tmp;
      engine.createLoggerState(nullptr, tmp);
      TRI_ASSERT(tmp.slice().isObject());

      // and now merge it into our response, while rewriting the "lastLogTick"
      // and "lastUncommittedLogTick"
      b.add("state", VPackValue(VPackValueType::Object));
      for (auto it : VPackObjectIterator(tmp.slice())) {
        if (it.key.stringView() == "lastLogTick" ||
            it.key.stringView() == "lastUncommittedLogTick") {
          // put into the tick from our own snapshot
          b.add(it.key.stringView(), VPackValue(snapTick));
        } else {
          // write our other attributes as they are
          b.add(it.key.stringView(), it.value);
        }
      }
      b.close();  // state
    }
    b.close();

    _vocbase.replicationClients().track(syncerId, clientId, clientInfo,
                                        ctx->snapshotTick(), ttl);

    generateResult(rest::ResponseCode::OK, b.slice());
    return;
  }

  if (type == rest::RequestType::PUT && len >= 2) {
    // extend an existing blocker
    auto id = static_cast<TRI_voc_tick_t>(StringUtils::uint64(suffixes[1]));

    bool parseSuccess = true;
    VPackSlice body = this->parseVPackBody(parseSuccess);
    if (!parseSuccess || !body.isObject()) {  // error already created
      return;
    }

    // extract ttl. Context uses initial ttl from batch creation, if `ttl == 0`
    auto ttl = VelocyPackHelper::getNumericValue<double>(
        body, "ttl", replutils::BatchInfo::DefaultTimeout);

    auto res = _manager->extendLifetime(id, ttl);
    if (res.fail()) {
      generateError(std::move(res).result());
      return;
    }

    SyncerId const syncerId = std::get<SyncerId>(res.get());
    arangodb::ServerId const clientId = std::get<arangodb::ServerId>(res.get());
    std::string const& clientInfo = std::get<std::string>(res.get());

    // last tick value in context should not have changed compared to the
    // initial tick value used in the context (it's only updated on bind()
    // call, which is only executed when a batch is initially created)
    _vocbase.replicationClients().extend(syncerId, clientId, clientInfo, ttl);

    resetResponse(rest::ResponseCode::NO_CONTENT);
    return;
  }

  if (type == rest::RequestType::DELETE_REQ && len >= 2) {
    // delete an existing blocker
    auto id = static_cast<TRI_voc_tick_t>(StringUtils::uint64(suffixes[1]));

    auto res = _manager->remove(id);
    if (res.fail()) {
      generateError(std::move(res).result());
      return;
    }

    resetResponse(rest::ResponseCode::NO_CONTENT);

    // extend client ttl by only a few seconds.
    // 15 seconds is enough time for the WAL tailing to kick in in case of
    // success, and in case of failure it is not too much additional time in
    // which WAL files are kept around
    static constexpr double extendPeriod = 15.0;

    SyncerId const syncerId = std::get<SyncerId>(res.get());
    arangodb::ServerId const clientId = std::get<arangodb::ServerId>(res.get());
    std::string const& clientInfo = std::get<std::string>(res.get());

    _vocbase.replicationClients().extend(syncerId, clientId, clientInfo,
                                         extendPeriod);
    return;
  }

  // we get here if anything above is invalid
  generateError(rest::ResponseCode::METHOD_NOT_ALLOWED,
                TRI_ERROR_HTTP_METHOD_NOT_ALLOWED);
}

void RocksDBRestReplicationHandler::handleCommandLoggerFollow() {
  bool useVst = false;
  if (_request->transportType() == Endpoint::TransportType::VST) {
    useVst = true;
  }

  // determine start and end tick
  TRI_voc_tick_t tickStart = 0;
  TRI_voc_tick_t tickEnd = UINT64_MAX;

  bool found;
  std::string const& value1 = _request->value("from", found);

  if (found) {
    tickStart = static_cast<TRI_voc_tick_t>(StringUtils::uint64(value1));
  }

  // determine end tick for dump
  std::string const& value2 = _request->value("to", found);
  if (found) {
    tickEnd = static_cast<TRI_voc_tick_t>(StringUtils::uint64(value2));
  }

  if (found && (tickStart > tickEnd || tickEnd == 0)) {
    generateError(rest::ResponseCode::BAD, TRI_ERROR_HTTP_BAD_PARAMETER,
                  "invalid from/to values");
    return;
  }

  // add client
  arangodb::ServerId const clientId{
      StringUtils::uint64(_request->value("serverId"))};
  SyncerId const syncerId = SyncerId::fromRequest(*_request);
  std::string const clientInfo = _request->value("clientInfo");

  bool includeSystem = _request->parsedValue("includeSystem", true);
  auto chunkSize = _request->parsedValue<uint64_t>("chunkSize", 1024 * 1024);

  ExecContextSuperuserScope escope(ExecContext::current().isAdminUser());

  // extract collection
  DataSourceId cid = DataSourceId::none();
  std::string const& value6 = _request->value("collection", found);
  if (found) {
    auto c = _vocbase.lookupCollection(value6);

    if (c == nullptr) {
      generateError(rest::ResponseCode::NOT_FOUND,
                    TRI_ERROR_ARANGO_DATA_SOURCE_NOT_FOUND);
      return;
    }

    cid = c->id();
  }

  auto trxContext = transaction::StandaloneContext::Create(_vocbase);
  VPackBuilder builder(trxContext->getVPackOptions());

  builder.openArray();

  auto result =
      tailWal(&_vocbase, tickStart, tickEnd, static_cast<size_t>(chunkSize),
              includeSystem, cid, builder);

  builder.close();

  auto data = builder.slice();

  auto& engine =
      server().getFeature<EngineSelectorFeature>().engine<RocksDBEngine>();
  uint64_t const latest = engine.db()->GetLatestSequenceNumber();

  if (result.fail()) {
    generateError(GeneralResponse::responseCode(result.errorNumber()),
                  result.errorNumber(), result.errorMessage());
    return;
  }

  TRI_ASSERT(latest >= result.maxTick());

  bool const checkMore = (result.maxTick() > 0 && result.maxTick() < latest);

  // generate the result
  size_t length = data.length();
  TRI_ASSERT(length == 0 || result.maxTick() > 0);

  if (length == 0) {
    resetResponse(rest::ResponseCode::NO_CONTENT);
  } else {
    resetResponse(rest::ResponseCode::OK);
  }

  // transfer ownership of the buffer contents
  _response->setContentType(rest::ContentType::DUMP);

  // set headers
  _response->setHeaderNC(StaticStrings::ReplicationHeaderCheckMore,
                         checkMore ? "true" : "false");
  _response->setHeaderNC(
      StaticStrings::ReplicationHeaderLastIncluded,
      StringUtils::itoa((length == 0) ? 0 : result.maxTick()));
  _response->setHeaderNC(StaticStrings::ReplicationHeaderLastTick,
                         StringUtils::itoa(latest));
  _response->setHeaderNC(StaticStrings::ReplicationHeaderLastScanned,
                         StringUtils::itoa(result.lastScannedTick()));
  _response->setHeaderNC(StaticStrings::ReplicationHeaderActive,
                         "true");  // TODO remove
  _response->setHeaderNC(StaticStrings::ReplicationHeaderFromPresent,
                         result.minTickIncluded() ? "true" : "false");

  if (length > 0) {
    if (useVst) {
      for (auto message : arangodb::velocypack::ArrayIterator(data)) {
        _response->addPayload(VPackSlice(message),
                              trxContext->getVPackOptions(), true);
      }
    } else {
      HttpResponse* httpResponse = dynamic_cast<HttpResponse*>(_response.get());

      if (httpResponse == nullptr) {
        THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_INTERNAL,
                                       "invalid response type");
      }

      basics::StringBuffer& buffer = httpResponse->body();
      basics::VPackStringBufferAdapter adapter(buffer.stringBuffer());
      // note: we need the CustomTypeHandler here
      velocypack::Dumper dumper(&adapter, trxContext->getVPackOptions());
      for (auto marker : arangodb::velocypack::ArrayIterator(data)) {
        dumper.dump(marker);
        httpResponse->body().appendChar('\n');
        // LOG_TOPIC("2c0b2", INFO, Logger::REPLICATION) <<
        // marker.toJson(trxContext->getVPackOptions());
      }
    }
  }

  // insert the start tick (minus 1 to be on the safe side) as the
  // minimum tick we need to keep on the master. we cannot be sure
  // the master's response makes it to the slave safely, so we must
  // not insert the maximum of the WAL entries we sent. if we did,
  // and the response does not make it to the slave, the master will
  // note a higher tick than the slave will have received, which may
  // lead to the master eventually deleting a WAL section that the
  // slave will still request later
  double ttl =
      _request->parsedValue("ttl", replutils::BatchInfo::DefaultTimeout);
  _vocbase.replicationClients().track(syncerId, clientId, clientInfo,
                                      tickStart == 0 ? 0 : tickStart - 1, ttl);
}

/// @brief run the command that determines which transactions were open at
/// a given tick value
/// this is an internal method use by ArangoDB's replication that should not
/// be called by client drivers directly
void RocksDBRestReplicationHandler::handleCommandDetermineOpenTransactions() {
  generateResult(rest::ResponseCode::OK, VPackSlice::emptyArraySlice());
  // rocksdb only includes finished transactions in the WAL.
  _response->setContentType(rest::ContentType::DUMP);
  _response->setHeaderNC(StaticStrings::ReplicationHeaderLastTick, "0");
  // always true to satisfy continuous syncer
  _response->setHeaderNC(StaticStrings::ReplicationHeaderFromPresent, "true");
}

void RocksDBRestReplicationHandler::handleCommandInventory() {
  bool found;
  std::string batchId = _request->value("batchId", found);
  if (!found) {
    generateError(rest::ResponseCode::NOT_FOUND, TRI_ERROR_CURSOR_NOT_FOUND,
                  "batchId not specified");
    return;
  }

  auto ctx = _manager->find(StringUtils::uint64(batchId));
  if (!ctx) {
    generateError(rest::ResponseCode::NOT_FOUND, TRI_ERROR_CURSOR_NOT_FOUND,
                  "context was not found");
    return;
  }

  TRI_voc_tick_t tick = TRI_CurrentTickServer();
  // include system collections?
  bool includeSystem = _request->parsedValue("includeSystem", true);
  bool includeFoxxQs = _request->parsedValue("includeFoxxQueues", false);

  // produce inventory for all databases?
  bool isGlobal = false;
  getApplier(isGlobal);

  // "collection" is optional, and may in the DB server case contain the name of
  // a single shard for shard synchronization
  std::string collection;
  if (!isGlobal) {
    collection = _request->value("collection");
  }

  VPackBuilder builder;
  builder.openObject();

  // add collections and views
  Result res;
  if (isGlobal) {
    builder.add(VPackValue("databases"));
    res = ctx->getInventory(_vocbase, includeSystem, includeFoxxQs, true,
                            builder);
  } else {
    ExecContextSuperuserScope escope(ExecContext::current().isAdminUser());
    if (collection.empty()) {
      // all collections in database
      res = ctx->getInventory(_vocbase, includeSystem, includeFoxxQs, false,
                              builder);
      TRI_ASSERT(builder.hasKey("collections") && builder.hasKey("views"));
    } else {
      // single collection/shard in database
      res = ctx->getInventory(_vocbase, collection, builder);
      TRI_ASSERT(builder.hasKey("collections"));
    }
  }

  if (res.fail()) {
    generateError(rest::ResponseCode::BAD, res.errorNumber(),
                  "inventory could not be created");
    return;
  }

  std::string const snapTick = std::to_string(ctx->snapshotTick());
  // <state>
  builder.add("state", VPackValue(VPackValueType::Object));
  builder.add("running", VPackValue(true));
  builder.add("lastLogTick", VPackValue(snapTick));
  builder.add("lastUncommittedLogTick", VPackValue(snapTick));
  builder.add("totalEvents", VPackValue(ctx->snapshotTick()));
  builder.add("time", VPackValue(utilities::timeString()));
  builder.close();  // </state>
  builder.add("tick", VPackValue(std::to_string(tick)));
  builder.close();  // Toplevel

  generateResult(rest::ResponseCode::OK, builder.slice());
}

/// @brief produce list of keys for a specific collection
/// If the call is made with option quick=true, and more than
/// 1 million documents are counted for keys, we'll return only
/// the document count, else, we proceed to deliver the keys.
void RocksDBRestReplicationHandler::handleCommandCreateKeys() {
  std::string const& collection = _request->value("collection");
  if (collection.empty()) {
    generateError(rest::ResponseCode::BAD, TRI_ERROR_HTTP_BAD_PARAMETER,
                  "invalid collection parameter");
    return;
  }

  std::string const& quick = _request->value("quick");
  if (!quick.empty() && !(quick == "true" || quick == "false")) {
    generateError(
        rest::ResponseCode::BAD, TRI_ERROR_HTTP_BAD_PARAMETER,
        std::string("invalid quick parameter: must be boolean, got ") + quick);
    return;
  }

  // to is ignored because the snapshot time is the latest point in time
  ExecContextSuperuserScope escope(ExecContext::current().isAdminUser());

  // get batchId from url parameters
  uint64_t batchId = _request->parsedValue("batchId", uint64_t(0));

  // find context
  auto ctx = _manager->find(batchId);
  if (!ctx) {
    generateError(rest::ResponseCode::NOT_FOUND, TRI_ERROR_CURSOR_NOT_FOUND,
                  "batchId not specified");
    return;
  }

  // bind collection to context - will initialize iterator
  auto [res, cid, numDocs] =
      ctx->bindCollectionIncremental(_vocbase, collection);
  if (res.fail()) {
    generateError(res);
    return;
  }

  if (numDocs > _quickKeysNumDocsLimit && quick == "true") {
    VPackBuilder result;
    result.add(VPackValue(VPackValueType::Object));
    result.add("count", VPackValue(numDocs));
    result.close();
    generateResult(rest::ResponseCode::OK, result.slice());
    return;
  }

  // keysId = <batchId>-<cid>
  std::string keysId = StringUtils::itoa(ctx->id());
  keysId.push_back('-');
  keysId.append(StringUtils::itoa(cid.id()));

  VPackBuilder result;
  result.add(VPackValue(VPackValueType::Object));
  result.add("id", VPackValue(keysId));
  result.add("count", VPackValue(numDocs));
  result.close();
  generateResult(rest::ResponseCode::OK, result.slice());
}

static std::pair<uint64_t, DataSourceId> extractBatchAndCid(
    std::string const& input) {
  auto pos = input.find('-');
  if (pos != std::string::npos && input.size() > pos + 1 && pos > 1) {
    return std::make_pair(
        StringUtils::uint64(input.c_str(), pos),
        DataSourceId{StringUtils::uint64(input.substr(pos + 1))});
  }
  return std::make_pair(0, DataSourceId::none());
}

/// @brief returns all key ranges
void RocksDBRestReplicationHandler::handleCommandGetKeys() {
  std::vector<std::string> const& suffixes = _request->suffixes();

  if (suffixes.size() != 2) {
    generateError(rest::ResponseCode::BAD, TRI_ERROR_HTTP_BAD_PARAMETER,
                  "expecting GET /_api/replication/keys/<keys-id>");
    return;
  }

  static uint64_t const DefaultChunkSize = 5000;

  // determine chunk size
  uint64_t chunkSize = _request->parsedValue("chunkSize", DefaultChunkSize);

  if (chunkSize < 100) {
    chunkSize = DefaultChunkSize;
  } else if (chunkSize > 20000) {
    chunkSize = 20000;
  }

  // first suffix needs to be the key id
  std::string const& keysId = suffixes[1];  // <batchId>-<cid>
  uint64_t batchId;
  DataSourceId cid;
  std::tie(batchId, cid) = extractBatchAndCid(keysId);

  // get context
  auto ctx = _manager->find(batchId);
  if (!ctx) {
    generateError(rest::ResponseCode::NOT_FOUND, TRI_ERROR_CURSOR_NOT_FOUND,
                  "batchId not specified, expired or invalid in another way");
    return;
  }

  VPackBuffer<uint8_t> buffer;
  VPackBuilder builder(buffer);
  ctx->dumpKeyChunks(_vocbase, cid, builder, chunkSize);
  generateResult(rest::ResponseCode::OK, std::move(buffer));
}

/// @brief returns date for a key range
void RocksDBRestReplicationHandler::handleCommandFetchKeys() {
  std::vector<std::string> const& suffixes = _request->suffixes();

  if (suffixes.size() != 2) {
    generateError(rest::ResponseCode::BAD, TRI_ERROR_HTTP_BAD_PARAMETER,
                  "expecting PUT /_api/replication/keys/<keys-id>");
    return;
  }

  static uint64_t const DefaultChunkSize = 5000;

  // determine chunk size
  uint64_t chunkSize = _request->parsedValue("chunkSize", DefaultChunkSize);
  if (chunkSize < 100) {
    chunkSize = DefaultChunkSize;
  } else if (chunkSize > 20000) {
    chunkSize = 20000;
  }

  // chunk is supplied by old clients, low is an optimization
  // for rocksdb, because seeking should be cheaper
  size_t chunk =
      static_cast<size_t>(_request->parsedValue("chunk", uint64_t(0)));

  bool found;
  std::string const& lowKey = _request->value("low", found);
  std::string const& value = _request->value("type", found);

  bool keys = true;
  if (value == "keys") {
    keys = true;
  } else if (value == "docs") {
    keys = false;
  } else {
    generateError(rest::ResponseCode::BAD, TRI_ERROR_HTTP_BAD_PARAMETER,
                  "invalid 'type' value");
    return;
  }

  // first suffix needs to be the key id
  std::string const& keysId = suffixes[1];  // <batchId>-<cid>
  uint64_t batchId;
  DataSourceId cid;
  std::tie(batchId, cid) = extractBatchAndCid(keysId);

  auto ctx = _manager->find(batchId);
  if (!ctx) {
    generateError(rest::ResponseCode::NOT_FOUND, TRI_ERROR_CURSOR_NOT_FOUND,
                  "batchId not specified or not found");
    return;
  }

  auto transactionContext = transaction::StandaloneContext::Create(_vocbase);
  VPackBuffer<uint8_t> buffer;
  VPackBuilder builder(buffer, transactionContext->getVPackOptions());

  if (keys) {
    Result rv = ctx->dumpKeys(_vocbase, cid, builder, chunk,
                              static_cast<size_t>(chunkSize), lowKey);
    if (rv.fail()) {
      generateError(rv);
      return;
    }
  } else {
    size_t offsetInChunk = 0;
    size_t maxChunkSize = SIZE_MAX;
    std::string const& value2 = _request->value("offset", found);
    if (found) {
      offsetInChunk = static_cast<size_t>(StringUtils::uint64(value2));
      // "offset" was introduced with ArangoDB 3.3. if the client sends it,
      // it means we can adapt the result size dynamically and the client
      // may refetch data for the same chunk
      maxChunkSize = 8 * 1024 * 1024;
      // if a client does not send an "offset" parameter at all, we are
      // not sure if it supports this protocol (3.2 and before) or not
    }

    bool success = false;
    VPackSlice const parsedIds = this->parseVPackBody(success);
    if (!success) {
      generateResult(rest::ResponseCode::BAD, VPackSlice());
      return;
    }

    Result rv = ctx->dumpDocuments(
        _vocbase, cid, builder, chunk, static_cast<size_t>(chunkSize),
        offsetInChunk, maxChunkSize, lowKey, parsedIds);

    if (rv.fail()) {
      generateError(rv);
      return;
    }
  }

  generateResult(rest::ResponseCode::OK, std::move(buffer), transactionContext);
}

void RocksDBRestReplicationHandler::handleCommandRemoveKeys() {
  std::vector<std::string> const& suffixes = _request->suffixes();

  if (suffixes.size() != 2) {
    generateError(rest::ResponseCode::BAD, TRI_ERROR_HTTP_BAD_PARAMETER,
                  "expecting DELETE /_api/replication/keys/<keys-id>");
    return;
  }

  // first suffix needs to be the key id
  std::string const& keysId = suffixes[1];  // <batchId>-<cid>
  auto [batchId, cid] = extractBatchAndCid(keysId);

  auto ctx = _manager->find(batchId);
  if (ctx) {
    ctx->releaseIterators(_vocbase, cid);
  }

  VPackBuilder resultBuilder;
  resultBuilder.openObject();
  resultBuilder.add("id", VPackValue(keysId));  // id as a string
  resultBuilder.add(StaticStrings::Error, VPackValue(false));
  resultBuilder.add(StaticStrings::Code,
                    VPackValue(static_cast<int>(rest::ResponseCode::ACCEPTED)));
  resultBuilder.close();

  generateResult(rest::ResponseCode::ACCEPTED, resultBuilder.slice());
}

void RocksDBRestReplicationHandler::handleCommandDump() {
  LOG_TOPIC("213e2", TRACE, arangodb::Logger::REPLICATION)
      << "enter handleCommandDump";

  bool found = false;
  uint64_t contextId = 0;

  // get collection Name
  std::string const& cname = _request->value("collection");
  if (cname.empty()) {
    generateError(rest::ResponseCode::BAD, TRI_ERROR_HTTP_BAD_PARAMETER,
                  "invalid collection parameter");
    return;
  }

  // get contextId
  std::string const& contextIdString = _request->value("batchId", found);
  if (found) {
    contextId = StringUtils::uint64(contextIdString);
  } else {
    generateError(rest::ResponseCode::NOT_FOUND, TRI_ERROR_HTTP_BAD_PARAMETER,
                  "replication dump - request misses batchId");
    return;
  }

  // acquire context
  auto ctx = _manager->find(contextId, /*ttl*/ 0);
  if (!ctx) {
    generateError(
        rest::ResponseCode::NOT_FOUND, TRI_ERROR_HTTP_BAD_PARAMETER,
        "replication dump - unable to find context (it could be expired)");
    return;
  }

  // print request
  LOG_TOPIC("2b20f", TRACE, arangodb::Logger::REPLICATION)
      << "requested collection dump for collection '" << collection
      << "' using contextId '" << ctx->id() << "'";

  ExecContextSuperuserScope escope(ExecContext::current().isAdminUser());

  if (!ExecContext::current().canUseCollection(_vocbase.name(), cname,
                                               auth::Level::RO)) {
    generateError(rest::ResponseCode::FORBIDDEN, TRI_ERROR_FORBIDDEN);
    return;
  }

  // "useEnvelope" URL parameter supported from >= 3.8 onwards. it defaults to
  // "true" if not set. when explicitly set to "false", we can get away with
  // using a more lightweight response format, in which each document does not
  // need to be wrapped into a {"type":2300,"data":<document>} envelope.
  bool const useEnvelope = _request->parsedValue("useEnvelope", true);

  // "array" URL parameter supported from >= 3.10 onwards. it defaults to
  // "false" if not set. when explictly set to "true", we can get away with
  // sending all documents as one big velocypack array, instead of sending
  // multiple velocypack documents one following another. this has the advantage
  // that on the client side we will receive a velocypack array which is ready
  // to be fed into a multi-document operation.
  bool const singleArray = _request->parsedValue("array", false);

  uint64_t chunkSize = determineChunkSize();
  size_t reserve = std::max<size_t>(chunkSize, 8192);

  RocksDBReplicationContext::DumpResult res(TRI_ERROR_NO_ERROR);
  if (_request->contentTypeResponse() == ContentType::VPACK) {
    VPackBuffer<uint8_t> buffer;
    buffer.reserve(reserve);  // avoid reallocs

    auto trxCtx = transaction::StandaloneContext::Create(_vocbase);

    res = ctx->dumpVPack(_vocbase, cname, buffer, chunkSize, useEnvelope,
                         singleArray);
    // generate the result
    if (res.fail()) {
      generateError(res.result());
    } else if (buffer.byteSize() == 0) {
      resetResponse(rest::ResponseCode::NO_CONTENT);
    } else {
      resetResponse(rest::ResponseCode::OK);
      _response->setContentType(rest::ContentType::VPACK);
      _response->setPayload(std::move(buffer), *trxCtx->getVPackOptions(),
                            /*resolveExternals*/ false);
    }

    // set headers
    _response->setHeaderNC(StaticStrings::ReplicationHeaderCheckMore,
                           (res.hasMore ? "true" : "false"));

    _response->setHeaderNC(
        StaticStrings::ReplicationHeaderLastIncluded,
        StringUtils::itoa(buffer.empty() ? 0 : res.includedTick));

  } else {
    StringBuffer dump(reserve, false);

    // do the work!
    res =
        ctx->dumpJson(_vocbase, cname, dump, determineChunkSize(), useEnvelope);

    if (res.fail()) {
      if (res.is(TRI_ERROR_BAD_PARAMETER)) {
        generateError(
            rest::ResponseCode::BAD, TRI_ERROR_HTTP_BAD_PARAMETER,
            StringUtils::concatT("replication dump - ", res.errorMessage()));
        return;
      }

      generateError(
          rest::ResponseCode::SERVER_ERROR, res.errorNumber(),
          StringUtils::concatT("replication dump - ", res.errorMessage()));
      return;
    }

    // generate the result
    if (dump.length() == 0) {
      resetResponse(rest::ResponseCode::NO_CONTENT);
    } else {
      resetResponse(rest::ResponseCode::OK);
    }

    _response->setContentType(rest::ContentType::DUMP);
    // set headers
    _response->setHeaderNC(StaticStrings::ReplicationHeaderCheckMore,
                           (res.hasMore ? "true" : "false"));
    _response->setHeaderNC(
        StaticStrings::ReplicationHeaderLastIncluded,
        StringUtils::itoa((dump.length() == 0) ? 0 : res.includedTick));

    if (_request->transportType() == Endpoint::TransportType::HTTP) {
      auto response = dynamic_cast<HttpResponse*>(_response.get());
      if (response == nullptr) {
        THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_INTERNAL,
                                       "invalid response type");
      }

      // transfer ownership of the buffer contents
      response->body().set(dump.stringBuffer());

      // avoid double freeing
      TRI_StealStringBuffer(dump.stringBuffer());
    } else {
      _response->addRawPayload(std::string_view(dump.data(), dump.length()));
      _response->setGenerateBody(true);
    }
  }
}

//////////////////////////////////////////////////////////////////////////////
/// @brief return the revision tree for a given collection, if available
//////////////////////////////////////////////////////////////////////////////

void RocksDBRestReplicationHandler::handleCommandRevisionTree() {
  RevisionOperationContext ctx;
  if (!prepareRevisionOperation(ctx)) {
    return;
  }

  // shall we do a verification?
  bool withVerification = _request->parsedValue("verification", false);

  // return only populated nodes in the tree (can make the result a lot
  // smaller and thus improve efficiency)
  bool onlyPopulated = _request->parsedValue("onlyPopulated", false);

  std::string collectionGuid = _request->value("collection");

  std::unique_ptr<containers::RevisionTree> tree;

  std::string_view errorReason = "did not find tree for snapshot";
  {
    auto c = _manager->find(ctx.batchId);
    if (c) {  // we have the RocksDBReplicationContext!
      // See if we can get the revision tree from the context:
      tree = c->getPrefetchedRevisionTree(collectionGuid);
      // This might return a nullptr!

      if (tree == nullptr) {  // Still not there, try to get it directly:
        tree = ctx.collection->getPhysical()->revisionTree(c->snapshotTick());
      }
      c->removeBlocker(_request->databaseName(), collectionGuid);
    } else {
      errorReason = "did not find batch";
    }
  }

  if (!tree) {
    TRI_ASSERT(!errorReason.empty());
    generateError(
        rest::ResponseCode::SERVER_ERROR, TRI_ERROR_INTERNAL,
        absl::StrCat("could not generate revision tree: ", errorReason));
    return;
  }

  VPackBuffer<uint8_t> buffer;
  VPackBuilder result(buffer);

  if (withVerification) {
    auto tree2 =
        ctx.collection->getPhysical()->computeRevisionTree(ctx.batchId);

    if (!tree2) {
      generateError(rest::ResponseCode::SERVER_ERROR, TRI_ERROR_INTERNAL,
                    "could not generate revision tree from collection");
      return;
    }

    VPackObjectBuilder guard(&result);
    result.add(VPackValue("computed"));
    tree2->serialize(result, onlyPopulated);
    result.add(VPackValue("stored"));
    tree->serialize(result, onlyPopulated);
    auto diff = tree->diff(*tree2);
    result.add("equal", VPackValue(diff.empty()));
  } else {
    tree->serialize(result, onlyPopulated);
  }

  generateResult(rest::ResponseCode::OK, std::move(buffer));
}

#ifdef ARANGODB_ENABLE_FAILURE_TESTS
/// @brief patch quickKeysNumDocsLimit for testing
void RocksDBRestReplicationHandler::adjustQuickKeysNumDocsLimit() {
  TRI_IF_FAILURE("RocksDBRestReplicationHandler::quickKeysNumDocsLimit100") {
    _quickKeysNumDocsLimit = 100;
  }
}
#endif
