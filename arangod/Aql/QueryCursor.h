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
/// @author Simon Grätzer
////////////////////////////////////////////////////////////////////////////////

#pragma once

#include "Aql/QueryResult.h"
#include "Aql/SharedAqlItemBlockPtr.h"
#include "Basics/Common.h"
#include "Transaction/Methods.h"
#include "Utils/Cursor.h"
#include "VocBase/vocbase.h"

#include <deque>

namespace arangodb {
namespace aql {

class AqlItemBlock;
enum class ExecutionState;
class Query;
class SharedAqlItemBlockPtr;

/// Cursor managing an entire query result in-memory
/// Should be used in conjunction with the RestCursorHandler
class QueryResultCursor final : public arangodb::Cursor {
 public:
  QueryResultCursor(TRI_vocbase_t& vocbase, aql::QueryResult&& result,
                    size_t batchSize, double ttl, bool hasCount,
                    bool isRetriable);

  ~QueryResultCursor() = default;

  aql::QueryResult const* result() const { return &_result; }

  bool hasNext();

  arangodb::velocypack::Slice next();

  size_t count() const override final;

  std::pair<aql::ExecutionState, Result> dump(
      velocypack::Builder& result) override final;

  Result dumpSync(velocypack::Builder& result) override final;

  std::shared_ptr<transaction::Context> context() const override final {
    return _result.context;
  }

  /// @brief Returns a slice to read the extra values.
  /// Make sure the Cursor Object is not destroyed while reading this slice.
  /// If no extras are set this will return a NONE slice.
  arangodb::velocypack::Slice extra() const;

  /// @brief Remember, if dirty reads were allowed:
  bool allowDirtyReads() const override final {
    return _result.allowDirtyReads;
  }

 private:
  DatabaseGuard _guard;
  aql::QueryResult _result;
  arangodb::velocypack::ArrayIterator _iterator;
  bool _cached;
};

/// Cursor managing a query from which it continuously gets
/// new results. Query, transaction and locks will live until
/// cursor is deleted (or query exhausted)
class QueryStreamCursor final : public arangodb::Cursor {
 public:
  QueryStreamCursor(std::shared_ptr<aql::Query> q, size_t batchSize, double ttl,
                    bool isRetriable);

  ~QueryStreamCursor();

  void kill() override;

  // Debug method to kill a query at a specific position
  // during execution. It internally asserts that the query
  // is actually visible through other APIS (e.g. current queries)
  // so user actually has a chance to kill it here.
  void debugKillQuery() override;

  size_t count() const override final { return 0; }

  std::pair<ExecutionState, Result> dump(
      velocypack::Builder& result) override final;

  Result dumpSync(velocypack::Builder& result) override final;

  /// Set wakeup callback on streaming cursor
  void setWakeupHandler(std::function<bool()> const& cb) override final;
  void resetWakeupHandler() override final;

  std::shared_ptr<transaction::Context> context() const override final;

  // The following method returns, if the transaction the query is using
  // allows dirty reads (reads from followers).
  virtual bool allowDirtyReads() const override final {
    // We got this information from the query directly in the constructor,
    // when `prepareQuery` has been called:
    return _allowDirtyReads;
  }

 private:
  // Writes from _queryResults to builder. Removes copied blocks from
  // _queryResults and sets _queryResultPos appropriately. Relies on the caller
  // to have fetched more than batchSize() result rows (if possible) in order to
  // set hasMore reliably.
  ExecutionState writeResult(arangodb::velocypack::Builder& builder);

  ExecutionState prepareDump();
  ExecutionState finalization();

  void cleanupStateCallback();

 private:
  velocypack::UInt8Buffer _extrasBuffer;
  std::deque<SharedAqlItemBlockPtr> _queryResults;  /// buffered results
  std::shared_ptr<transaction::Context> _ctx;       /// cache context
  std::shared_ptr<aql::Query> _query;
  /// index of the next to-be-returned row in _queryResults.front()
  size_t _queryResultPos;

  /// used when cursor is owned by V8 transaction
  transaction::Methods::StatusChangeCallback _stateChangeCb;

  bool _finalization;

  bool _allowDirtyReads;  // keep this information when the query is already
                          // gone.
};

}  // namespace aql
}  // namespace arangodb
