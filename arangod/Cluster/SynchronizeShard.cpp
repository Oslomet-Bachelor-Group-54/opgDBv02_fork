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
/// @author Kaveh Vahedipour
/// @author Matthew Von-Maszewski
////////////////////////////////////////////////////////////////////////////////

#include "SynchronizeShard.h"

#include "Agency/AgencyStrings.h"
#include "ApplicationFeatures/ApplicationServer.h"
#include "Basics/ScopeGuard.h"
#include "Basics/StringUtils.h"
#include "Basics/TimeString.h"
#include "Basics/VelocyPackHelper.h"
#include "Basics/debugging.h"
#include "Cluster/ActionDescription.h"
#include "Cluster/AgencyCache.h"
#include "Cluster/ClusterFeature.h"
#include "Cluster/ClusterInfo.h"
#include "Cluster/CollectionInfoCurrent.h"
#include "Cluster/FollowerInfo.h"
#include "Cluster/Maintenance.h"
#include "Cluster/MaintenanceFeature.h"
#include "Cluster/ReplicationTimeoutFeature.h"
#include "Cluster/ServerState.h"
#include "GeneralServer/AuthenticationFeature.h"
#include "Logger/LogMacros.h"
#include "Metrics/Counter.h"
#include "Network/Methods.h"
#include "Network/NetworkFeature.h"
#include "Network/Utils.h"
#include "Replication/DatabaseInitialSyncer.h"
#include "Replication/DatabaseReplicationApplier.h"
#include "Replication/DatabaseTailingSyncer.h"
#include "Replication/ReplicationApplierConfiguration.h"
#include "Replication/ReplicationFeature.h"
#include "RestServer/DatabaseFeature.h"
#include "RestServer/ServerIdFeature.h"
#include "StorageEngine/EngineSelectorFeature.h"
#include "StorageEngine/PhysicalCollection.h"
#include "StorageEngine/StorageEngine.h"
#include "Transaction/StandaloneContext.h"
#include "Utils/CollectionNameResolver.h"
#include "Utils/SingleCollectionTransaction.h"
#include "VocBase/LogicalCollection.h"
#include "VocBase/Methods/Collections.h"
#include "VocBase/Methods/Databases.h"

#include <velocypack/Compare.h>
#include <velocypack/Iterator.h>
#include <velocypack/Slice.h>

using namespace arangodb::application_features;
using namespace arangodb::maintenance;
using namespace arangodb::methods;
using namespace arangodb::transaction;
using namespace arangodb;
using namespace arangodb::basics;
using namespace arangodb::consensus;

std::string const ENDPOINT("endpoint");
std::string const INCLUDE("include");
std::string const INCLUDE_SYSTEM("includeSystem");
std::string const INCREMENTAL("incremental");
std::string const LEADER_ID("leaderId");
std::string const LAST_LOG_TICK("lastLogTick");
std::string const API_REPLICATION("/_api/replication/");
std::string const REPL_ADD_FOLLOWER(API_REPLICATION + "addFollower");
std::string const REPL_HOLD_READ_LOCK(API_REPLICATION +
                                      "holdReadLockCollection");
std::string const REPL_REM_FOLLOWER(API_REPLICATION + "removeFollower");
std::string const RESTRICT_TYPE("restrictType");
std::string const RESTRICT_COLLECTIONS("restrictCollections");
std::string const SKIP_CREATE_DROP("skipCreateDrop");
std::string const TTL("ttl");

using namespace std::chrono;

// Overview over the code in this file:
// The main method being called is "first", it does:
// first:
//  - wait until leader has created shard
//  - lookup local shard
//  - call `replicationSynchronize`
//  - call `catchupWithReadLock`
//  - call `catchupWithExclusiveLock`
// replicationSynchronize:
//  - set local shard to follow leader (without a following term id)
//  - use a `DatabaseInitialSyncer` to synchronize to a certain state,
//    (configure leaderId for it to go through)
// catchupWithReadLock:
//  - start a read lock on leader
//  - keep configuration for shard to follow the leader without term id
//  - do WAL tailing with read-lock (configure leaderId
//    for it to go through)
//  - cancel read lock on leader
// catchupWithExclusiveLock:
//  - start an exclusive lock on leader, acquire unique following term id
//  - set local shard to follower leader (with new following term id)
//  - call `replicationSynchronizeFinalize` (WAL tailing)
//  - do a final check by comparing counts on leader and follower
//  - add us as official follower on the leader
//  - release exclusive lock on leader
//

SynchronizeShard::SynchronizeShard(MaintenanceFeature& feature,
                                   ActionDescription const& desc)
    : ActionBase(feature, desc),
      ShardDefinition(desc.get(DATABASE), desc.get(SHARD)),
      _followingTermId(0),
      _tailingUpperBoundTick(0),
      _initialDocCountOnLeader(0),
      _initialDocCountOnFollower(0),
      _docCountAtEnd(0) {
  std::stringstream error;

  if (!desc.has(COLLECTION)) {
    error << "collection must be specified. ";
  }
  TRI_ASSERT(desc.has(COLLECTION));

  if (!ShardDefinition::isValid()) {
    error << "database and shard must be specified. ";
  }

  if (!desc.has(THE_LEADER) || desc.get(THE_LEADER).empty()) {
    error << "leader must be specified and must be non-empty. ";
  }
  TRI_ASSERT(desc.has(THE_LEADER) && !desc.get(THE_LEADER).empty());

  if (!desc.has(SHARD_VERSION)) {
    error << "local shard version must be specified. ";
  }
  TRI_ASSERT(desc.has(SHARD_VERSION));

  if (!error.str().empty()) {
    LOG_TOPIC("03780", ERR, Logger::MAINTENANCE)
        << "SynchronizeShard: " << error.str();
    result(TRI_ERROR_INTERNAL, error.str());
    setState(FAILED);
  }
}

std::string const& SynchronizeShard::clientInfoString() const {
  return _clientInfoString;
}

SynchronizeShard::~SynchronizeShard() = default;

static std::stringstream& AppendShardInformationToMessage(
    std::string const& database, std::string const& shard,
    std::string const& planId,
    std::chrono::system_clock::time_point const& startTime,
    std::stringstream& msg) {
  auto const endTime = std::chrono::system_clock::now();
  msg << "local shard: '" << database << "/" << shard << "', "
      << "for central: '" << database << "/" << planId << "', "
      << "started: " << timepointToString(startTime) << ", "
      << "ended: " << timepointToString(endTime);
  return msg;
}

static arangodb::Result getReadLockId(network::ConnectionPool* pool,
                                      std::string const& endpoint,
                                      std::string const& database,
                                      std::string const& clientId,
                                      double timeout, uint64_t& id) {
  TRI_ASSERT(timeout > 0);

  if (pool == nullptr) {  // nullptr only happens during controlled shutdown
    return arangodb::Result(TRI_ERROR_SHUTTING_DOWN,
                            "startReadLockOnLeader: Shutting down");
  }

  std::string error("startReadLockOnLeader: Failed to get read lock");

  network::RequestOptions options;
  options.database = database;
  options.timeout = network::Timeout(timeout);
  options.skipScheduler = true;  // hack to speed up future.get()

  auto response =
      network::sendRequest(pool, endpoint, fuerte::RestVerb::Get,
                           REPL_HOLD_READ_LOCK, VPackBuffer<uint8_t>(), options)
          .get();
  auto res = response.combinedResult();

  if (res.ok()) {
    VPackSlice idSlice = response.slice();
    TRI_ASSERT(idSlice.isObject());
    TRI_ASSERT(idSlice.hasKey(ID));

    try {
      id = std::stoull(idSlice.get(ID).copyString());
    } catch (std::exception const&) {
      error += " - expecting id to be uint64_t ";
      error += idSlice.toJson();
      res.reset(TRI_ERROR_INTERNAL, error);
    } catch (...) {
      TRI_ASSERT(false);
      res.reset(TRI_ERROR_INTERNAL, error);
    }
  }

  return res;
}

arangodb::Result collectionReCount(LogicalCollection& collection, uint64_t& c) {
  Result res;
  try {
    c = collection.getPhysical()->recalculateCounts();
  } catch (basics::Exception const& e) {
    res.reset(e.code(), e.message());
  }
  return res;
}

static arangodb::Result addShardFollower(
    network::ConnectionPool* pool, std::string const& endpoint,
    std::string const& database, std::string const& shard, uint64_t lockJobId,
    std::string const& clientId, SyncerId const syncerId,
    std::string const& clientInfoString, double timeout,
    uint64_t& docCountAtEnd) {
  if (pool == nullptr) {  // nullptr only happens during controlled shutdown
    return arangodb::Result(TRI_ERROR_SHUTTING_DOWN,
                            "startReadLockOnLeader: Shutting down");
  }

  LOG_TOPIC("b982e", DEBUG, Logger::MAINTENANCE)
      << "addShardFollower: tell the leader to put us into the follower "
         "list for "
      << database << "/" << shard << "...";

  try {
    auto& df =
        pool->config().clusterInfo->server().getFeature<DatabaseFeature>();
    DatabaseGuard guard(df, database);
    auto vocbase = &guard.database();

    auto collection = vocbase->lookupCollection(shard);
    if (collection == nullptr) {
      std::string errorMsg(
          "SynchronizeShard::addShardFollower: Failed to lookup collection ");
      errorMsg += database + "/" + shard;
      LOG_TOPIC("4a8db", ERR, Logger::MAINTENANCE) << errorMsg;
      return arangodb::Result(TRI_ERROR_ARANGO_DATA_SOURCE_NOT_FOUND, errorMsg);
    }

    uint64_t docCount;
    Result res = arangodb::maintenance::collectionCount(*collection, docCount);
    if (res.fail()) {
      return res;
    }

    docCountAtEnd = docCount;

    VPackBuilder body;
    {
      VPackObjectBuilder b(&body);
      body.add(FOLLOWER_ID,
               VPackValue(arangodb::ServerState::instance()->getId()));
      body.add(SHARD, VPackValue(shard));
      body.add("checksum", VPackValue(std::to_string(docCount)));
      body.add(
          "serverId",
          VPackValue(basics::StringUtils::itoa(ServerIdFeature::getId().id())));
      if (syncerId.value != 0) {
        body.add("syncerId", VPackValue(syncerId.toString()));
      }
      if (!clientInfoString.empty()) {
        body.add("clientInfo", VPackValue(clientInfoString));
      }
      if (lockJobId != 0) {
        body.add("readLockId", VPackValue(std::to_string(lockJobId)));
      }
    }

    network::RequestOptions options;
    options.database = database;
    options.timeout = network::Timeout(timeout);
    options.skipScheduler = true;  // hack to speed up future.get()

    auto response = network::sendRequest(pool, endpoint, fuerte::RestVerb::Put,
                                         REPL_ADD_FOLLOWER,
                                         std::move(*body.steal()), options)
                        .get();
    auto result = response.combinedResult();

    if (result.fail()) {
      auto const errorMessage =
          "addShardFollower: could not add us to the leader's follower list "
          "for " +
          database + "/" + shard;

      if (lockJobId != 0) {
        LOG_TOPIC("22e0a", WARN, Logger::MAINTENANCE)
            << errorMessage << ", " << result.errorMessage();
      } else {
        LOG_TOPIC("abf2e", INFO, Logger::MAINTENANCE)
            << errorMessage << " with shortcut (can happen, no problem).";
        if (result.errorNumber() == TRI_ERROR_REPLICATION_SHARD_NONEMPTY) {
          return result;  // hand on leader protest
        }
      }
      return arangodb::Result(
          result.errorNumber(),
          StringUtils::concatT(errorMessage, ", ", result.errorMessage()));
    }
    LOG_TOPIC("79935", DEBUG, Logger::MAINTENANCE)
        << "addShardFollower: success";
    return arangodb::Result();
  } catch (std::exception const& e) {
    std::string errorMsg(
        "SynchronizeShard::addShardFollower: Failed to lookup database ");
    errorMsg += database;
    errorMsg += " exception: ";
    errorMsg += e.what();
    LOG_TOPIC("6b7e8", ERR, Logger::MAINTENANCE) << errorMsg;
    return arangodb::Result(TRI_ERROR_ARANGO_DATABASE_NOT_FOUND, errorMsg);
  }
}

static arangodb::Result cancelReadLockOnLeader(network::ConnectionPool* pool,
                                               std::string const& endpoint,
                                               std::string const& database,
                                               uint64_t lockJobId,
                                               std::string const& clientId,
                                               double timeout) {
  TRI_ASSERT(timeout > 0.0);

  if (pool == nullptr) {  // nullptr only happens during controlled shutdown
    return arangodb::Result(TRI_ERROR_SHUTTING_DOWN,
                            "cancelReadLockOnLeader: Shutting down");
  }

  VPackBuilder body;
  {
    VPackObjectBuilder b(&body);
    body.add(ID, VPackValue(std::to_string(lockJobId)));
  }

  network::RequestOptions options;
  options.database = database;
  options.timeout = network::Timeout(timeout);
  options.skipScheduler = true;  // hack to speed up future.get()

  auto response = network::sendRequest(pool, endpoint, fuerte::RestVerb::Delete,
                                       REPL_HOLD_READ_LOCK,
                                       std::move(*body.steal()), options)
                      .get();

  auto res = response.combinedResult();
  if (res.is(TRI_ERROR_ARANGO_DATABASE_NOT_FOUND)) {
    // database is gone. that means our lock is also gone
    return arangodb::Result();
  }

  if (res.fail()) {
    // rebuild body since we stole it earlier
    VPackBuilder body;
    {
      VPackObjectBuilder b(&body);
      body.add(ID, VPackValue(std::to_string(lockJobId)));
    }
    LOG_TOPIC("52924", WARN, Logger::MAINTENANCE)
        << "cancelReadLockOnLeader: exception caught for " << body.toJson()
        << ": " << res.errorMessage();
    return arangodb::Result(TRI_ERROR_INTERNAL, res.errorMessage());
  }

  LOG_TOPIC("4355c", DEBUG, Logger::MAINTENANCE)
      << "cancelReadLockOnLeader: success";
  return arangodb::Result();
}

arangodb::Result SynchronizeShard::collectionCountOnLeader(
    std::string const& leaderEndpoint, uint64_t& docCountOnLeader) {
  NetworkFeature& nf = _feature.server().getFeature<NetworkFeature>();
  network::ConnectionPool* pool = nf.pool();
  network::RequestOptions options;
  options.database = getDatabase();
  options.timeout = network::Timeout(60);
  options.skipScheduler = true;  // hack to speed up future.get()
  network::Headers headers;
  headers.insert_or_assign("X-Arango-Frontend", "true");

  auto response =
      network::sendRequest(pool, leaderEndpoint, fuerte::RestVerb::Get,
                           "/_api/collection/" + getShard() + "/count",
                           VPackBuffer<uint8_t>(), options, std::move(headers))
          .get();
  auto res = response.combinedResult();
  if (res.fail()) {
    docCountOnLeader = 0;
    return res;
  }
  VPackSlice body = response.slice();
  TRI_ASSERT(body.isObject());
  TRI_ASSERT(body.hasKey("count"));
  VPackSlice count = body.get("count");
  TRI_ASSERT(count.isNumber());
  try {
    docCountOnLeader = count.getNumber<uint64_t>();
  } catch (std::exception const& exc) {
    return {TRI_ERROR_INTERNAL, exc.what()};
  }
  return {};
}

arangodb::Result SynchronizeShard::getReadLock(network::ConnectionPool* pool,
                                               std::string const& endpoint,
                                               std::string const& collection,
                                               std::string const& clientId,
                                               uint64_t rlid, bool soft,
                                               double timeout) {
  TRI_ASSERT(timeout > 0.0);

  // This function can be implemented in a more robust manner for server
  // versions > 3.4. Starting with 3.4 the POST requests to the read lock API
  // terminates the server side thread as soon as the lock request comes in.
  // The POST request thus is answered immediately back to the caller.
  // The servers (<=3.3) with lower versions hold the POST request for as long
  // as the corresponding DELETE_REQ has not been successfully submitted.

  // nullptr only happens during controlled shutdown
  if (pool == nullptr) {
    return arangodb::Result(TRI_ERROR_SHUTTING_DOWN,
                            "getReadLock: Shutting down");
  }

  VPackBuilder body;
  {
    VPackObjectBuilder o(&body);
    body.add(ID, VPackValue(std::to_string(rlid)));
    body.add(COLLECTION, VPackValue(collection));
    body.add(TTL, VPackValue(timeout));
    body.add("serverId",
             VPackValue(arangodb::ServerState::instance()->getId()));
    body.add(StaticStrings::RebootId,
             VPackValue(ServerState::instance()->getRebootId().value()));
    body.add(StaticStrings::ReplicationSoftLockOnly, VPackValue(soft));
    // the following attribute was added in 3.8.3:
    // with this, the follower indicates to the leader that it is
    // capable of handling following term ids correctly.
    bool sendWantFollowingTerm = true;
    TRI_IF_FAILURE("SynchronizeShard::dontSendWantFollowingTerm") {
      sendWantFollowingTerm = false;
    }
    if (sendWantFollowingTerm) {
      body.add("wantFollowingTerm", VPackValue(true));
    }
  }
  auto buf = body.steal();

  // Try to POST the lock body. If POST fails, we should just exit and retry
  // SynchronizeShard anew.
  network::RequestOptions options;
  options.timeout = network::Timeout(timeout);
  options.database = getDatabase();
  // In the hardLock case we need to continue as fast as possible
  // and cannot be allowed to be blocked by overloading of the server.
  // This operation now holds an exclusive lock on the leading server
  // which will make overloading situation worse.
  // So we want to bypass the scheduler here.
  options.skipScheduler = !soft;

  auto response = network::sendRequest(pool, endpoint, fuerte::RestVerb::Post,
                                       REPL_HOLD_READ_LOCK, *buf, options)
                      .get();

  auto res = response.combinedResult();

  if (res.ok()) {
    // Habemus clausum, we have a lock
    if (!soft) {
      // Now store the random followingTermId:
      VPackSlice body = response.response().slice();
      if (body.isObject()) {
        VPackSlice followingTermIdSlice =
            body.get(StaticStrings::FollowingTermId);
        if (followingTermIdSlice.isNumber()) {
          _followingTermId = followingTermIdSlice.getNumber<uint64_t>();
        }
        // check if the leader sent us a "lastLogTick" value.
        // if yes, we pick it up and use it as an upper bound until
        // which we at most need to do WAL tailing under the exclusive
        // lock
        VPackSlice lastLogTickSlice = body.get("lastLogTick");
        if (lastLogTickSlice.isNumber()) {
          _tailingUpperBoundTick = lastLogTickSlice.getNumber<uint64_t>();
        }
      }
    }
    return {};
  }

  LOG_TOPIC("cba32", DEBUG, Logger::MAINTENANCE)
      << "startReadLockOnLeader: couldn't POST lock body, "
      << network::fuerteToArangoErrorMessage(response) << ", giving up.";

  // We MUSTN'T exit without trying to clean up a lock that was maybe acquired
  if (response.error == fuerte::Error::CouldNotConnect) {
    return arangodb::Result(
        TRI_ERROR_INTERNAL,
        "startReadLockOnLeader: couldn't POST lock body, giving up.");
  }

  // Ambiguous POST, we'll try to DELETE a potentially acquired lock
  try {
    auto cancelResponse =
        network::sendRequest(pool, endpoint, fuerte::RestVerb::Delete,
                             REPL_HOLD_READ_LOCK, *buf, options)
            .get();
    auto cancelRes = cancelResponse.combinedResult();
    if (cancelRes.fail()) {
      LOG_TOPIC("4f34d", WARN, Logger::MAINTENANCE)
          << "startReadLockOnLeader: cancelation error for shard "
          << getDatabase() << "/" << collection << ": "
          << cancelRes.errorMessage();
    }
  } catch (std::exception const& e) {
    LOG_TOPIC("7fcc9", WARN, Logger::MAINTENANCE)
        << "startReadLockOnLeader: exception in cancel: " << e.what();
  }

  // original response that we received when ordering the lock
  TRI_ASSERT(res.fail());
  return res;
}

arangodb::Result SynchronizeShard::startReadLockOnLeader(
    std::string const& endpoint, std::string const& collection,
    std::string const& clientId, uint64_t& rlid, bool soft, double timeout) {
  TRI_ASSERT(timeout > 0);
  // Read lock id
  rlid = 0;
  NetworkFeature& nf = _feature.server().getFeature<NetworkFeature>();
  network::ConnectionPool* pool = nf.pool();
  arangodb::Result result =
      getReadLockId(pool, endpoint, getDatabase(), clientId, timeout, rlid);
  if (!result.ok()) {
    LOG_TOPIC("2e5ae", WARN, Logger::MAINTENANCE) << result.errorMessage();
  } else {
    LOG_TOPIC("c8d18", DEBUG, Logger::MAINTENANCE)
        << "Got read lock id: " << rlid;

    result.reset(
        getReadLock(pool, endpoint, collection, clientId, rlid, soft, timeout));
  }

  return result;
}

static arangodb::ResultT<SyncerId> replicationSynchronize(
    SynchronizeShard& job,
    std::chrono::time_point<std::chrono::steady_clock> endTime,
    std::shared_ptr<arangodb::LogicalCollection> const& col, VPackSlice config,
    std::shared_ptr<DatabaseTailingSyncer> tailingSyncer, VPackBuilder& sy) {
  auto& vocbase = col->vocbase();
  auto database = vocbase.name();

  std::string leaderId;
  if (config.hasKey(LEADER_ID)) {
    leaderId = config.get(LEADER_ID).copyString();
  }

  ReplicationApplierConfiguration configuration =
      ReplicationApplierConfiguration::fromVelocyPack(vocbase.server(), config,
                                                      database);
  configuration.setClientInfo(job.clientInfoString());
  configuration.validate();

  // database-specific synchronization
  auto syncer = DatabaseInitialSyncer::create(vocbase, configuration);

  if (!leaderId.empty()) {
    // In this phase we use the normal leader ID without following term id:
    syncer->setLeaderId(leaderId);
  }

  syncer->setOnSuccessCallback(
      [tailingSyncer](DatabaseInitialSyncer& syncer) -> Result {
        // store leader info for later, so that the next phases don't need to
        // acquire it again. this saves an HTTP roundtrip to the leader when
        // initializing the WAL tailing.
        return tailingSyncer->inheritFromInitialSyncer(syncer);
      });

  auto& agencyCache =
      job.feature().server().getFeature<ClusterFeature>().agencyCache();
  ReplicationTimeoutFeature& timeouts =
      job.feature().server().getFeature<ReplicationTimeoutFeature>();

  syncer->setCancellationCheckCallback([=, &agencyCache, &timeouts]() -> bool {
    // Will return true if the SynchronizeShard job should be aborted.
    LOG_TOPIC("39856", DEBUG, Logger::REPLICATION)
        << "running synchronization cancelation check for shard " << database
        << "/" << col->name();
    if (endTime.time_since_epoch().count() > 0 &&
        std::chrono::steady_clock::now() >= endTime) {
      // configured timeout exceeded
      LOG_TOPIC("47154", INFO, Logger::REPLICATION)
          << "stopping initial sync attempt for " << database << "/"
          << col->name() << " after configured timeout of "
          << timeouts.shardSynchronizationAttemptTimeout() << " s. "
          << "a new sync attempt will be scheduled...";
      return true;
    }

    std::string path = "Plan/Collections/" + database + "/" +
                       std::to_string(col->planId().id()) + "/shards/" +
                       col->name();
    VPackBuilder builder;
    agencyCache.get(builder, path);

    if (!builder.isEmpty()) {
      VPackSlice plan = builder.slice();
      if (plan.isArray() && plan.length() >= 2) {
        if (plan[0].isString() && plan[0].isEqualString(leaderId)) {
          std::string myself = arangodb::ServerState::instance()->getId();
          for (size_t i = 1; i < plan.length(); ++i) {
            if (plan[i].isString() && plan[i].isEqualString(myself)) {
              // do not abort the synchronization
              return false;
            }
          }
        }
      }
    }

    // abort synchronization
    LOG_TOPIC("f6dbc", INFO, Logger::REPLICATION)
        << "aborting initial sync for " << database << "/" << col->name()
        << " because we are not planned as a follower anymore";
    return true;
  });

  SyncerId syncerId{syncer->syncerId()};

  try {
    std::string const context = "syncing shard " + database + "/" + col->name();
    Result r = syncer->run(configuration._incremental, context.c_str());

    if (r.fail()) {
      LOG_TOPIC("3efff", DEBUG, Logger::REPLICATION)
          << "initial sync failed for " << database << "/" << col->name()
          << ": " << r.errorMessage();
      return r;
    }

    {
      VPackObjectBuilder o(&sy);
      sy.add(LAST_LOG_TICK, VPackValue(syncer->getLastLogTick()));
      sy.add(VPackValue(COLLECTIONS));
      {
        VPackArrayBuilder a(&sy);
        for (auto const& i : syncer->getProcessedCollections()) {
          VPackObjectBuilder e(&sy);
          sy.add(ID, VPackValue(i.first.id()));
          sy.add(NAME, VPackValue(i.second));
        }
      }
    }

  } catch (arangodb::basics::Exception const& ex) {
    std::string s("cannot sync from remote endpoint: ");
    s += ex.what() + std::string(". last progress message was '") +
         syncer->progress() + "'";
    return Result(ex.code(), s);
  } catch (std::exception const& ex) {
    std::string s("cannot sync from remote endpoint: ");
    s += ex.what() + std::string(". last progress message was '") +
         syncer->progress() + "'";
    return Result(TRI_ERROR_INTERNAL, s);
  } catch (...) {
    std::string s(
        "cannot sync from remote endpoint: unknown exception. last progress "
        "message was '");
    s += syncer->progress() + "'";
    return Result(TRI_ERROR_INTERNAL, s);
  }

  return ResultT<SyncerId>::success(syncerId);
}

bool SynchronizeShard::first() {
  TRI_IF_FAILURE("SynchronizeShard::disable") { return false; }

  std::string const& database = getDatabase();
  std::string const& planId = _description.get(COLLECTION);
  std::string const& shard = getShard();
  std::string const& leader = _description.get(THE_LEADER);
  bool forcedResync = _description.has(FORCED_RESYNC) &&
                      _description.get(FORCED_RESYNC) == "true";
  bool syncByRevision = _description.has(SYNC_BY_REVISION) &&
                        _description.get(SYNC_BY_REVISION) == "true";

  size_t failuresInRow = _feature.replicationErrors(database, shard);

  // from this many number of failures in a row, we will step on the brake
  constexpr size_t delayThreshold = 4;

  if (failuresInRow >= delayThreshold) {
    // shard synchronization has failed several times in a row.
    // now step on the brake a bit. this blocks our maintenance thread, but
    // currently there seems to be no better way to delay the execution of
    // maintenance tasks.
    double sleepTime = 2.0 + 0.1 * (failuresInRow * (failuresInRow + 1) / 2);

    // cap sleep time to 15 seconds
    sleepTime = std::min<double>(sleepTime, 15.0);

    LOG_TOPIC("40376", INFO, Logger::MAINTENANCE)
        << "SynchronizeShard: synchronizing shard '" << database << "/" << shard
        << "' for central '" << database << "/" << planId << "' encountered "
        << failuresInRow << " failures in a row. delaying next sync by "
        << sleepTime << " s";

    TRI_IF_FAILURE("SynchronizeShard::noSleepOnSyncError") { sleepTime = 0.0; }

    while (sleepTime > 0.0) {
      if (_feature.server().isStopping()) {
        result(TRI_ERROR_SHUTTING_DOWN);
        return false;
      }

      constexpr double sleepPerRound = 0.5;
      // sleep only for up to 0.5 seconds at a time so we can react quickly to
      // shutdown
      std::this_thread::sleep_for(
          std::chrono::duration<double>(std::min(sleepTime, sleepPerRound)));
      sleepTime -= sleepPerRound;
    }
  }

  LOG_TOPIC("fa651", DEBUG, Logger::MAINTENANCE)
      << "SynchronizeShard: synchronizing shard '" << database << "/" << shard
      << "' for central '" << database << "/" << planId << "'";

  auto& clusterInfo =
      _feature.server().getFeature<ClusterFeature>().clusterInfo();
  auto const ourselves = arangodb::ServerState::instance()->getId();
  auto startTime = std::chrono::system_clock::now();
  auto const startTimeStr = timepointToString(startTime);
  std::string const clientId(database + planId + shard + leader);

  // First wait until the leader has created the shard (visible in
  // Current in the Agency) or we or the shard have vanished from
  // the plan:
  while (true) {
    if (_feature.server().isStopping()) {
      result(TRI_ERROR_SHUTTING_DOWN);
      return false;
    }

    std::vector<std::string> planned;
    auto res = clusterInfo.getShardServers(shard, planned);

    if (!res.ok() ||
        std::find(planned.begin(), planned.end(), ourselves) == planned.end() ||
        planned.front() != leader) {
      // Things have changed again, simply terminate:
      std::stringstream error;
      error << "cancelled, ";
      AppendShardInformationToMessage(database, shard, planId, startTime,
                                      error);
      LOG_TOPIC("a1dc7", DEBUG, Logger::MAINTENANCE)
          << "SynchronizeOneShard: " << error.str();
      result(TRI_ERROR_FAILED, error.str());
      return false;
    }

    auto ci = clusterInfo.getCollectionNT(database, planId);
    if (ci == nullptr) {
      std::stringstream msg;
      msg << "exception in getCollection, ";
      AppendShardInformationToMessage(database, shard, planId, startTime, msg);
      LOG_TOPIC("89972", DEBUG, Logger::MAINTENANCE)
          << "SynchronizeOneShard: " << msg.str();
      result(TRI_ERROR_FAILED, msg.str());
      return false;
    }

    std::string const cid = std::to_string(ci->id().id());
    std::shared_ptr<CollectionInfoCurrent> cic =
        clusterInfo.getCollectionCurrent(database, cid);
    std::vector<std::string> current = cic->servers(shard);

    if (current.empty()) {
      LOG_TOPIC("b0ccf", DEBUG, Logger::MAINTENANCE)
          << "synchronizeOneShard: cancelled, no servers in 'Current'";
    } else if (current.front() == leader) {
      if (std::find(current.begin(), current.end(), ourselves) ==
          current.end()) {
        break;  // start synchronization work
      }
      // This was the normal case. However, if we have been away for a short
      // amount of time and the leader has not yet noticed that we were gone,
      // we might actually get here and try to resync and are still in
      // Current. In this case, we write a log message and sync anyway:
      std::stringstream error;
      if (forcedResync) {
        error << "found ourselves in Current, but resyncing anyways because of "
                 "a recent restart, ";
        AppendShardInformationToMessage(database, shard, planId, startTime,
                                        error);
        LOG_TOPIC("4abcd", DEBUG, Logger::MAINTENANCE)
            << "SynchronizeOneShard: " << error.str();
        break;
      }
      // Otherwise, we give up on the job, since we do not want to repeat
      // a SynchronizeShard if we are already in Current:
      error << "already done, ";
      AppendShardInformationToMessage(database, shard, planId, startTime,
                                      error);
      LOG_TOPIC("4abcb", DEBUG, Logger::MAINTENANCE)
          << "SynchronizeOneShard: " << error.str();
      result(TRI_ERROR_FAILED, error.str());
      return false;
    } else {
      // we need to immediately exit, as the planned leader is not yet leading
      // in current
      LOG_TOPIC("4acdc", DEBUG, Logger::MAINTENANCE)
          << "SynchronizeOneShard: Planned leader has not taken over "
             "leadership";
      result(TRI_ERROR_FAILED, "Planned leader has not taken over leadership");
      return false;
    }

    LOG_TOPIC("28600", DEBUG, Logger::MAINTENANCE)
        << "synchronizeOneShard: waiting for leader, " << database << "/"
        << shard << ", " << database << "/" << planId;

    std::this_thread::sleep_for(duration<double>(0.2));
  }

  // Once we get here, we know that the leader is ready for sync, so we give it
  // a try:

  try {
    auto& df = _feature.server().getFeature<DatabaseFeature>();
    DatabaseGuard guard(df, database);
    auto vocbase = &guard.database();

    auto collection = vocbase->lookupCollection(shard);
    if (collection == nullptr) {
      std::stringstream error;
      error << "failed to lookup local shard " << database << "/" << shard;
      LOG_TOPIC("06489", ERR, Logger::MAINTENANCE)
          << "SynchronizeOneShard: " << error.str();
      result(TRI_ERROR_ARANGO_DATA_SOURCE_NOT_FOUND, error.str());
      return false;
    }

    auto ep = clusterInfo.getServerEndpoint(leader);
    uint64_t docCountOnLeader = 0;
    if (Result res = collectionCountOnLeader(ep, docCountOnLeader);
        res.fail()) {
      std::stringstream error;
      error << "failed to get a count on leader " << database << "/" << shard
            << ": " << res.errorMessage();
      LOG_TOPIC("1254a", ERR, Logger::MAINTENANCE)
          << "SynchronizeShard " << error.str();
      result(res.errorNumber(), error.str());
      return false;
    }

    _initialDocCountOnLeader = docCountOnLeader;

    uint64_t docCount = 0;
    if (Result res = collectionCount(*collection, docCount); res.fail()) {
      std::stringstream error;
      error << "failed to get a count here " << database << "/" << shard << ": "
            << res.errorMessage();
      LOG_TOPIC("da225", ERR, Logger::MAINTENANCE)
          << "SynchronizeShard " << error.str();
      result(res.errorNumber(), error.str());
      return false;
    }

    _initialDocCountOnFollower = docCount;

    if (_priority != maintenance::SLOW_OP_PRIORITY &&
        docCount != docCountOnLeader &&
        ((docCount < docCountOnLeader && docCountOnLeader - docCount > 10000) ||
         (docCount > docCountOnLeader &&
          docCount - docCountOnLeader > 10000))) {
      // This could be a larger job, let's reschedule ourselves with
      // priority SLOW_OP_PRIORITY:
      LOG_TOPIC("25a62", DEBUG, Logger::MAINTENANCE)
          << "SynchronizeShard action found that leader's and follower's "
             "document count differ by more than 10000, will reschedule with "
             "slow priority, database: "
          << database << ", shard: " << shard;
      requeueMe(maintenance::SLOW_OP_PRIORITY);
      result(TRI_ERROR_ACTION_UNFINISHED,
             "SynchronizeShard action rescheduled to slow operation priority");
      return false;
    }

    {  // Initialize _clientInfoString
      CollectionNameResolver resolver(collection->vocbase());
      _clientInfoString = std::string{"follower "} +
                          ServerState::instance()->getId() + " of shard " +
                          database + "/" + collection->name() +
                          " of collection " + database + "/" +
                          resolver.getCollectionName(collection->id());
    }

    // determine end timestamp for shard synchronization attempt, if any
    if (syncByRevision) {
      // note: we can only set the timeout if we can use the Merkle-tree
      // based synchronization protocol. this protocol can work incrementally
      // and can make progress within limited time even if the number of
      // documents in the underlying shard is very large.
      // the pre-Merkle tree protocol requires a setup time proportional
      // to the number of documents in the collection, and may not make
      // progress within the configured timeout value.
      ReplicationTimeoutFeature& timeouts =
          _feature.server().getFeature<ReplicationTimeoutFeature>();
      double attemptTimeout = timeouts.shardSynchronizationAttemptTimeout();
      if (attemptTimeout > 0.0) {
        // set end time for synchronization attempt
        _endTimeForAttempt =
            std::chrono::steady_clock::now() +
            std::chrono::seconds(static_cast<int64_t>(attemptTimeout));
      }
    }

    LOG_TOPIC("53337", DEBUG, Logger::MAINTENANCE)
        << "synchronizeOneShard: trying to synchronize local shard '"
        << database << "/" << shard << "' for central '" << database << "/"
        << planId << "'";

    // the destructor of the tailingSyncer will automatically unregister itself
    // from the leader in case it still has to do so (it will do it at most once
    // per tailingSyncer object, and only if the tailingSyncer registered itself
    // on the leader)
    std::shared_ptr<DatabaseTailingSyncer> tailingSyncer =
        buildTailingSyncer(guard.database(), ep);

    // tailingSyncer cannot be a nullptr here, because
    // DatabaseTailingSyncer::create() returns the result of a make_shared
    // operation.
    TRI_ASSERT(tailingSyncer != nullptr);

    try {
      // From here on we perform a number of steps, each of which can
      // fail. If it fails with an exception, it is caught, but this
      // should usually not happen. If it fails without an exception,
      // we log and use return.

      // First once without a read transaction:

      if (_feature.server().isStopping()) {
        std::string errorMessage(
            "SynchronizeShard: synchronization failed for shard ");
        errorMessage += shard + ": shutdown in progress, giving up";
        LOG_TOPIC("a0f9a", INFO, Logger::MAINTENANCE) << errorMessage;
        result(TRI_ERROR_SHUTTING_DOWN, errorMessage);
        return false;
      }

      VPackBuilder config;
      {
        VPackObjectBuilder o(&config);
        config.add(ENDPOINT, VPackValue(ep));
        config.add(
            INCREMENTAL,
            VPackValue(docCount > 0));  // use incremental sync if possible
        config.add(LEADER_ID, VPackValue(leader));
        config.add(SKIP_CREATE_DROP, VPackValue(true));
        config.add(RESTRICT_TYPE, VPackValue(INCLUDE));
        config.add(VPackValue(RESTRICT_COLLECTIONS));
        {
          VPackArrayBuilder a(&config);
          config.add(VPackValue(shard));
        }
        config.add(INCLUDE_SYSTEM, VPackValue(true));
        config.add("verbose", VPackValue(false));
      }

      // Configure the shard to follow the leader without any following
      // term id:
      collection->followers()->setTheLeader(leader);

      startTime = std::chrono::system_clock::now();

      VPackBuilder builder;
      ResultT<SyncerId> syncRes =
          replicationSynchronize(*this, _endTimeForAttempt, collection,
                                 config.slice(), tailingSyncer, builder);

      auto const endTime = std::chrono::system_clock::now();

      // Long shard sync initialization
      if (endTime - startTime > seconds(5)) {
        LOG_TOPIC("ca7e3", INFO, Logger::MAINTENANCE)
            << "synchronizeOneShard: long call to syncCollection for shard"
            << database << "/" << shard << " " << syncRes.errorMessage()
            << " start time: " << timepointToString(startTime)
            << ", end time: " << timepointToString(endTime);
      }

      // If this did not work, then we cannot go on:
      if (!syncRes.ok()) {
        if (_endTimeForAttempt.time_since_epoch().count() > 0 &&
            std::chrono::steady_clock::now() >= _endTimeForAttempt) {
          // we reached the configured timeout.
          // rebrand the error. this is important because this is a special
          // error that does not count towards the "failed" attempts.
          syncRes =
              Result(TRI_ERROR_REPLICATION_SHARD_SYNC_ATTEMPT_TIMEOUT_EXCEEDED);
        }

        std::stringstream error;
        error << "could not initially synchronize shard " << database << "/"
              << shard << ": " << syncRes.errorMessage();
        LOG_TOPIC("c1b31", DEBUG, Logger::MAINTENANCE)
            << "SynchronizeOneShard: " << error.str();

        result(syncRes.errorNumber(), error.str());
        return false;
      }

      SyncerId syncerId = syncRes.get();

      VPackSlice sy = builder.slice();
      VPackSlice collections = sy.get(COLLECTIONS);
      if (collections.length() == 0 ||
          collections[0].get("name").stringView() != shard) {
        std::stringstream error;
        error
            << "shard " << database << "/" << shard
            << " seems to be gone from leader, this "
               "can happen if a collection was dropped during synchronization!";
        LOG_TOPIC("664ae", WARN, Logger::MAINTENANCE)
            << "SynchronizeOneShard: " << error.str();
        result(TRI_ERROR_INTERNAL, error.str());
        return false;
      }

      ReplicationTimeoutFeature& timeouts =
          _feature.server().getFeature<ReplicationTimeoutFeature>();

      tailingSyncer->setCancellationCheckCallback(
          [=, endTime = _endTimeForAttempt, &timeouts]() -> bool {
            // Will return true if the tailing syncer should be aborted.
            LOG_TOPIC("54ec2", DEBUG, Logger::REPLICATION)
                << "running tailing cancelation check for shard " << database
                << "/" << collection->name();
            if (endTime.time_since_epoch().count() > 0 &&
                std::chrono::steady_clock::now() >= endTime) {
              // configured timeout exceeded
              LOG_TOPIC("66e75", INFO, Logger::REPLICATION)
                  << "stopping tailing sync attempt for " << database << "/"
                  << collection->name() << " after configured timeout of "
                  << timeouts.shardSynchronizationAttemptTimeout() << " s. "
                  << "a new sync attempt will be scheduled...";
              return true;
            }

            return false;
          });

      auto lastTick =
          arangodb::basics::VelocyPackHelper::getNumericValue<TRI_voc_tick_t>(
              sy, LAST_LOG_TICK, 0);

      ResultT<TRI_voc_tick_t> tickResult = catchupWithReadLock(
          ep, *collection, clientId, leader, lastTick, tailingSyncer);

      if (!tickResult.ok()) {
        LOG_TOPIC("0a4d4", INFO, Logger::MAINTENANCE)
            << tickResult.errorMessage();
        result(std::move(tickResult).result());
        return false;
      }
      lastTick = tickResult.get();

      // Now start an exclusive transaction to stop writes:
      Result res = catchupWithExclusiveLock(ep, *collection, clientId, leader,
                                            syncerId, lastTick, tailingSyncer);
      if (!res.ok()) {
        LOG_TOPIC("be85f", INFO, Logger::MAINTENANCE) << res.errorMessage();
        result(res);
        return false;
      }
    } catch (basics::Exception const& e) {
      // don't log errors for already dropped databases/collections
      if (e.code() != TRI_ERROR_ARANGO_DATABASE_NOT_FOUND &&
          e.code() != TRI_ERROR_ARANGO_DATA_SOURCE_NOT_FOUND) {
        std::stringstream error;
        error << "synchronization of ";
        AppendShardInformationToMessage(database, shard, planId, startTime,
                                        error);
        error << " failed: " << e.what();
        LOG_TOPIC("65d6f", ERR, Logger::MAINTENANCE) << error.str();
      }
      result(e.code(), e.what());
      return false;
    } catch (std::exception const& e) {
      std::stringstream error;
      error << "synchronization of ";
      AppendShardInformationToMessage(database, shard, planId, startTime,
                                      error);
      error << " failed: " << e.what();
      LOG_TOPIC("1e576", ERR, Logger::MAINTENANCE) << error.str();
      result(TRI_ERROR_INTERNAL, e.what());
      return false;
    }
    // Validate that HARDLOCK only works!
  } catch (std::exception const& e) {
    // This catches the case that we could not even find the collection
    // locally, because the DatabaseGuard constructor threw.
    LOG_TOPIC("9f2c0", WARN, Logger::MAINTENANCE)
        << "action " << _description << " failed with exception " << e.what();
    result(TRI_ERROR_INTERNAL, e.what());
    return false;
  }

  // Tell others that we are done:
  if (Logger::isEnabled(LogLevel::INFO, Logger::MAINTENANCE)) {
    // This wrap is just to not write the stream if not needed.
    std::stringstream msg;
    AppendShardInformationToMessage(database, shard, planId, startTime, msg);
    LOG_TOPIC("e6780", DEBUG, Logger::MAINTENANCE)
        << "synchronizeOneShard: done, " << msg.str();
  }
  return false;
}

ResultT<TRI_voc_tick_t> SynchronizeShard::catchupWithReadLock(
    std::string const& ep, LogicalCollection const& collection,
    std::string const& clientId, std::string const& leader,
    TRI_voc_tick_t lastLogTick,
    std::shared_ptr<DatabaseTailingSyncer> tailingSyncer) {
  TRI_ASSERT(lastLogTick > 0);
  TRI_ASSERT(tailingSyncer != nullptr);

  bool didTimeout = true;
  int tries = 0;
  double timeout = 300.0;
  TRI_voc_tick_t tickReached = 0;
  while (didTimeout && tries++ < 18) {  // This will try to sync for at most ~1
                                        // hour. ((300 * 0.6) * 18 == 3240)
    if (_feature.server().isStopping()) {
      std::string errorMessage =
          "SynchronizeShard: startReadLockOnLeader (soft): shutting down";
      return ResultT<TRI_voc_tick_t>::error(TRI_ERROR_SHUTTING_DOWN,
                                            errorMessage);
    }

    didTimeout = false;
    // Now ask for a "soft stop" on the leader, in case of mmfiles, this
    // will be a hard stop, but for rocksdb, this is a no-op:
    uint64_t lockJobId = 0;
    LOG_TOPIC("b4f2b", DEBUG, Logger::MAINTENANCE)
        << "synchronizeOneShard: startReadLockOnLeader (soft): " << ep << ":"
        << getDatabase() << ":" << collection.name();
    Result res = startReadLockOnLeader(ep, collection.name(), clientId,
                                       lockJobId, true, timeout);
    if (!res.ok()) {
      auto errorMessage = StringUtils::concatT(
          "SynchronizeShard: error in startReadLockOnLeader (soft):",
          res.errorMessage());
      return ResultT<TRI_voc_tick_t>::error(res.errorNumber(),
                                            std::move(errorMessage));
    }

    auto readLockGuard = arangodb::scopeGuard([&, this]() noexcept {
      try {
        // Always cancel the read lock.
        // Reported seperately
        NetworkFeature& nf = _feature.server().getFeature<NetworkFeature>();
        network::ConnectionPool* pool = nf.pool();
        auto res = cancelReadLockOnLeader(pool, ep, getDatabase(), lockJobId,
                                          clientId, 60.0);
        if (!res.ok()) {
          LOG_TOPIC("b15ee", INFO, Logger::MAINTENANCE)
              << "Could not cancel soft read lock on leader: "
              << res.errorMessage();
        }
      } catch (std::exception const& ex) {
        LOG_TOPIC("e32be", ERR, Logger::MAINTENANCE)
            << "Failed to cancel soft read lock on leader: " << ex.what();
      }
    });

    LOG_TOPIC("5eb37", DEBUG, Logger::MAINTENANCE)
        << "lockJobId: " << lockJobId;

    // From now on, we need to cancel the read lock on the leader regardless
    // if things go wrong or right!

    // Do a first try of a catch up with the WAL. In case of RocksDB,
    // this has not yet stopped the writes, so we have to be content
    // with nearly reaching the end of the WAL, which is a "soft" catchup.

    // We only allow to hold this lock for 60% of the timeout time, so to avoid
    // any issues with Locks timeouting on the Leader and the Client not
    // recognizing it.

    try {
      didTimeout = false;
      std::string const context = "catching up delta changes for shard " +
                                  getDatabase() + "/" + collection.name();
      res = tailingSyncer->syncCollectionCatchup(collection.name(), lastLogTick,
                                                 timeout * 0.6, tickReached,
                                                 didTimeout, context);
    } catch (std::exception const& ex) {
      res = {TRI_ERROR_INTERNAL, ex.what()};
    }

    if (!res.ok()) {
      std::string errorMessage(
          "synchronizeOneShard: error in syncCollectionCatchup for shard ");
      errorMessage += getDatabase() + "/" + getShard() + ": ";
      errorMessage += res.errorMessage();
      return ResultT<TRI_voc_tick_t>::error(res.errorNumber(), errorMessage);
    }

    // Stop the read lock again:
    NetworkFeature& nf = _feature.server().getFeature<NetworkFeature>();
    network::ConnectionPool* pool = nf.pool();
    res = cancelReadLockOnLeader(pool, ep, getDatabase(), lockJobId, clientId,
                                 60.0);
    // We removed the readlock
    readLockGuard.cancel();
    if (!res.ok()) {
      auto errorMessage = StringUtils::concatT(
          "synchronizeOneShard: error when cancelling soft read lock: ",
          res.errorMessage());
      LOG_TOPIC("c37d1", INFO, Logger::MAINTENANCE) << errorMessage;
      result(TRI_ERROR_INTERNAL, errorMessage);
      return ResultT<TRI_voc_tick_t>::error(TRI_ERROR_INTERNAL, errorMessage);
    }
    lastLogTick = tickReached;
    if (didTimeout) {
      LOG_TOPIC("e516e", INFO, Logger::MAINTENANCE)
          << "Renewing softLock for " << getShard() << " on leader: " << leader;
    }
  }
  if (didTimeout) {
    LOG_TOPIC("f1a61", WARN, Logger::MAINTENANCE)
        << "Could not catchup under softLock for " << getShard()
        << " on leader: " << leader
        << " now activating hardLock. This is expected under high load.";
  }
  return ResultT<TRI_voc_tick_t>::success(tickReached);
}

Result SynchronizeShard::catchupWithExclusiveLock(
    std::string const& ep, LogicalCollection& collection,
    std::string const& clientId, std::string const& leader,
    SyncerId const syncerId, TRI_voc_tick_t lastLogTick,
    std::shared_ptr<DatabaseTailingSyncer> tailingSyncer) {
  TRI_ASSERT(tailingSyncer != nullptr);

  uint64_t lockJobId = 0;
  LOG_TOPIC("da129", DEBUG, Logger::MAINTENANCE)
      << "synchronizeOneShard: startReadLockOnLeader: " << ep << ":"
      << getDatabase() << ":" << collection.name();

  // we should not yet have an upper bound for WAL tailing.
  // the next call to startReadLockOnLeader may set it if the leader already
  // implements it (ArangoDB 3.8.3 and higher)
  TRI_ASSERT(_tailingUpperBoundTick == 0);
  TRI_IF_FAILURE("FollowerBlockRequestsLanesForSyncOnShard" +
                 collection.name()) {
    TRI_AddFailurePointDebugging("BlockSchedulerMediumQueue");
  }
  Result res =
      startReadLockOnLeader(ep, collection.name(), clientId, lockJobId, false);
  if (!res.ok()) {
    auto errorMessage = StringUtils::concatT(
        "SynchronizeShard: error in startReadLockOnLeader (hard): ",
        res.errorMessage());
    return {res.errorNumber(), std::move(errorMessage)};
  }
  auto readLockGuard = arangodb::scopeGuard([&, this]() noexcept {
    try {
      // Always cancel the read lock.
      // Reported seperately
      NetworkFeature& nf = _feature.server().getFeature<NetworkFeature>();
      network::ConnectionPool* pool = nf.pool();
      auto res = cancelReadLockOnLeader(pool, ep, getDatabase(), lockJobId,
                                        clientId, 60.0);
      if (!res.ok()) {
        LOG_TOPIC("067a8", INFO, Logger::MAINTENANCE)
            << "Could not cancel hard read lock on leader: "
            << res.errorMessage();
      }
    } catch (std::exception const& ex) {
      LOG_TOPIC("d7848", ERR, Logger::MAINTENANCE)
          << "Failed to cancel hard read lock on leader: " << ex.what();
    }
  });

  // Now we have got a unique id for this following term and have stored it
  // in _followingTermId, so we can use it to set the leader:

  // This is necessary to accept replications from the leader which can
  // happen as soon as we are in sync.
  std::string leaderIdWithTerm{leader};
  if (_followingTermId != 0) {
    leaderIdWithTerm += "_";
    leaderIdWithTerm += basics::StringUtils::itoa(_followingTermId);
  }
  // If _followingTermId is 0, then this is a leader before the update,
  // we tolerate this and simply use its ID without a term in this case.
  collection.followers()->setTheLeader(leaderIdWithTerm);
  LOG_TOPIC("d76cb", DEBUG, Logger::MAINTENANCE) << "lockJobId: " << lockJobId;

  // repurpose tailingSyncer
  tailingSyncer->setLeaderId(leaderIdWithTerm);

  try {
    std::string const context =
        "finalizing shard " + getDatabase() + "/" + collection.name();
    res = tailingSyncer->syncCollectionFinalize(
        collection.name(), lastLogTick, _tailingUpperBoundTick, context);
  } catch (std::exception const& ex) {
    res = {TRI_ERROR_INTERNAL, ex.what()};
  }

  if (!res.ok()) {
    std::string errorMessage(
        "synchronizeOneshard: error in syncCollectionFinalize for shard ");
    errorMessage += getDatabase() + "/" + collection.name() + ": ";
    errorMessage += res.errorMessage();
    return {res.errorNumber(), errorMessage};
  }

  NetworkFeature& nf = _feature.server().getFeature<NetworkFeature>();
  network::ConnectionPool* pool = nf.pool();
  res =
      addShardFollower(pool, ep, getDatabase(), getShard(), lockJobId, clientId,
                       syncerId, _clientInfoString, 60.0, _docCountAtEnd);

  TRI_IF_FAILURE("SynchronizeShard::wrongChecksum") {
    res.reset(TRI_ERROR_REPLICATION_WRONG_CHECKSUM);
  }

  // if we get a checksum mismatch, it means that we got different counts of
  // documents on the leader and the follower, which can happen if collection
  // counts are off for whatever reason.
  // under many cicrumstances the counts will have been auto-healed by the
  // initial or the incremental replication before, so in many cases we will not
  // even get into this if case
  if (res.is(TRI_ERROR_REPLICATION_WRONG_CHECKSUM)) {
    // give up the lock on the leader, so writes aren't stopped unncessarily
    // on the leader while we are recalculating the counts
    readLockGuard.fire();

    ++collection.vocbase()
          .server()
          .getFeature<ClusterFeature>()
          .followersWrongChecksumCounter();

    // recalculate collection count on follower
    LOG_TOPIC("29384", INFO, Logger::MAINTENANCE)
        << "recalculating collection count on follower for " << getDatabase()
        << "/" << getShard();

    uint64_t docCount = 0;
    Result countRes = collectionCount(collection, docCount);
    if (countRes.fail()) {
      return countRes;
    }
    // store current count value
    uint64_t oldCount = docCount;

    // recalculate on follower. this can take a long time
    countRes = collectionReCount(collection, docCount);
    if (countRes.fail()) {
      return countRes;
    }

    LOG_TOPIC("d2689", INFO, Logger::MAINTENANCE)
        << "recalculated collection count on follower for " << getDatabase()
        << "/" << getShard() << ", old: " << oldCount << ", new: " << docCount;

    // check if our recalculation has made a difference
    if (oldCount == docCount) {
      // no change happened due to recalculation. now try recounting on leader
      // too. this is last resort and should not happen often!
      LOG_TOPIC("3dc64", INFO, Logger::MAINTENANCE)
          << "recalculating collection count on leader for " << getDatabase()
          << "/" << getShard();

      VPackBuffer<uint8_t> buffer;
      VPackBuilder tmp(buffer);
      tmp.add(VPackSlice::emptyObjectSlice());

      network::RequestOptions options;
      options.database = getDatabase();
      options.timeout = network::Timeout(900.0);  // this can be slow!!!
      options.skipScheduler = true;  // hack to speed up future.get()

      std::string const url = "/_api/collection/" +
                              StringUtils::urlEncode(collection.name()) +
                              "/recalculateCount";

      // send out the request
      auto future = network::sendRequest(pool, ep, fuerte::RestVerb::Put, url,
                                         std::move(buffer), options);

      network::Response const& r = future.get();

      Result result = r.combinedResult();

      if (result.fail()) {
        auto const errorMessage = StringUtils::concatT(
            "addShardFollower: could not add us to the leader's follower list "
            "for ",
            getDatabase(), "/", getShard(),
            ", error while recalculating count on leader: ",
            result.errorMessage());
        LOG_TOPIC("22e0b", WARN, Logger::MAINTENANCE) << errorMessage;
        return arangodb::Result(result.errorNumber(), std::move(errorMessage));
      } else {
        auto const resultSlice = r.slice();
        if (VPackSlice c = resultSlice.get("count"); c.isNumber()) {
          LOG_TOPIC("bc26d", DEBUG, Logger::MAINTENANCE)
              << "leader's shard count response is " << c.getNumber<uint64_t>();
        }
      }
    }

    // still let the operation fail here, because we gave up the lock
    // already and cannot be sure the data on the leader hasn't changed in
    // the meantime. we will sort this issue out during the next maintenance
    // run
    TRI_ASSERT(res.fail());
    TRI_ASSERT(res.is(TRI_ERROR_REPLICATION_WRONG_CHECKSUM));
    return res;
  }

  // no more retrying...
  if (!res.ok()) {
    std::string errorMessage(
        "synchronizeOneshard: error in addShardFollower: ");
    errorMessage += res.errorMessage();
    return {res.errorNumber(), errorMessage};
  }

  // Report success:
  LOG_TOPIC("3423d", DEBUG, Logger::MAINTENANCE)
      << "synchronizeOneShard: synchronization worked for shard " << getShard();
  result(TRI_ERROR_NO_ERROR);
  return {};
}

void SynchronizeShard::setState(ActionState state) {
  if ((COMPLETE == state || FAILED == state) && _state != state) {
    bool haveRequeued = result().is(TRI_ERROR_ACTION_UNFINISHED);
    // This error happens if we abort the action because we assumed
    // that it would take too long. In this case it has been rescheduled
    // and we must not unlock the shard!
    // We also do not report the error in the agency.

    // by all means we must unlock when we leave this scope
    auto shardUnlocker = scopeGuard([this, haveRequeued]() noexcept {
      if (!haveRequeued) {
        _feature.unlockShard(getShard());
      }
    });

    if (COMPLETE == state) {
      LOG_TOPIC("50827", INFO, Logger::MAINTENANCE)
          << "SynchronizeShard: synchronization completed for shard "
          << getDatabase() << "/" << getShard()
          << ", initial document count on leader: " << _initialDocCountOnLeader
          << ", initial document count on follower: "
          << _initialDocCountOnFollower
          << ", document count at end: " << _docCountAtEnd;

      // because we succeeded now, we can wipe out all past failures
      _feature.removeReplicationError(getDatabase(), getShard());
    } else {
      TRI_ASSERT(FAILED == state);

      // check if we have hit the configured shard synchronization attempt
      // timeout. if so, this does not count as an error
      bool isTimeoutExceeded = result().is(
          TRI_ERROR_REPLICATION_SHARD_SYNC_ATTEMPT_TIMEOUT_EXCEEDED);
      if (!haveRequeued && !isTimeoutExceeded) {
        // increase failure counter for this shard. if we have accumulated
        // x many failures in a row, the shard on the follower will be
        // dropped and completely rebuilt.
        _feature.storeReplicationError(getDatabase(), getShard());
      }
      if (isTimeoutExceeded) {
        // track the number of timeouts
        _feature.countTimedOutSyncAttempt();
      }
    }

    // Acquire current version from agency and wait for it to have been dealt
    // with in local current cache. Any future current version will do, as
    // the version is incremented by the leader ahead of getting here on the
    // follower.
    uint64_t v = 0;
    auto timeout = std::chrono::duration<double>(600.0);
    auto stoppage = std::chrono::steady_clock::now() + timeout;
    auto snooze = std::chrono::milliseconds(100);
    while (!_feature.server().isStopping() &&
           std::chrono::steady_clock::now() < stoppage) {
      cluster::fetchCurrentVersion(0.1 * timeout)
          .thenValue([&v](auto&& res) {
            // we need to check if res is ok() in order to not trigger a
            // bad_optional_access exception here
            if (res.ok()) {
              v = res.get();
            }
          })
          .thenError<std::exception>([this](std::exception const& e) {
            LOG_TOPIC("3ae99", ERR, Logger::CLUSTER)
                << "Failed to acquire current version from agency while "
                   "increasing shard version"
                << " for shard " << getDatabase() << "/" << getShard() << ": "
                << e.what();
          })
          .wait();
      if (v > 0) {
        break;
      }
      std::this_thread::sleep_for(snooze);
      if (snooze < std::chrono::seconds(2)) {
        snooze += std::chrono::milliseconds(100);
      }
    }

    // We're here, cause we either ran out of time or have an actual version
    // number. In the former case, we tried our best and will safely continue
    // some 10 min later. If however v is an actual positive integer, we'll wait
    // for it to sync in out ClusterInfo cache through loadCurrent.
    if (v > 0) {
      _feature.server()
          .getFeature<ClusterFeature>()
          .clusterInfo()
          .waitForCurrentVersion(v)
          .wait();
    }
    _feature.incShardVersion(getShard());
  }
  ActionBase::setState(state);
}

std::shared_ptr<DatabaseTailingSyncer> SynchronizeShard::buildTailingSyncer(
    TRI_vocbase_t& vocbase, std::string const& endpoint) {
  // build configuration for WAL tailing
  ReplicationApplierConfiguration configuration(_feature.server());
  configuration._endpoint = endpoint;
  configuration._database = getDatabase();
  configuration._requestTimeout = 600.0;
  configuration._connectTimeout = 30.0;
  // set JWT
  if (_feature.server().hasFeature<AuthenticationFeature>()) {
    configuration._jwt = _feature.server()
                             .getFeature<AuthenticationFeature>()
                             .tokenCache()
                             .jwtToken();
  }
  // will throw if invalid
  configuration.validate();

  // build DatabaseTailingSyncer object for WAL tailing
  auto syncer = DatabaseTailingSyncer::create(vocbase, configuration,
                                              /*lastTick*/ 0, /*useTick*/ true);

  std::string const& leader = _description.get(THE_LEADER);
  if (!leader.empty()) {
    // In the initial phase we still use the normal leaderId without a following
    // term id:
    syncer->setLeaderId(leader);
  }

  return syncer;
}
