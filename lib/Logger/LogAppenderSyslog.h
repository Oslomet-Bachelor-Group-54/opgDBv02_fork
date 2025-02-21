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
/// @author Dr. Frank Celler
////////////////////////////////////////////////////////////////////////////////

#pragma once

#include "Logger/LogAppender.h"

namespace arangodb {
#ifdef ARANGODB_ENABLE_SYSLOG

class LogAppenderSyslog final : public LogAppender {
 public:
  LogAppenderSyslog(std::string const& facility, std::string const& name);

  void logMessage(LogMessage const& message) override final;

  std::string details() const override final;

  static void close();

 private:
  std::string const _sysname;

  static bool _opened;
};
#endif
}  // namespace arangodb
