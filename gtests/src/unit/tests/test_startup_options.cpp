/*
  Copyright (c) DataStax, Inc.

  Licensed under the Apache License, Version 2.0 (the "License");
  you may not use this file except in compliance with the License.
  You may obtain a copy of the License at

  http://www.apache.org/licenses/LICENSE-2.0

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.
*/

#include "unit.hpp"

#include "driver_info.hpp"
#include "query_request.hpp"
#include "session.hpp"


#define APPLICATION_NAME "DataStax C/C++ Test Harness"
#define APPLICATION_VERSION "1.0.0"

class StartupRequestUnitTest : public Unit {
public:
  void TearDown() {
    ASSERT_TRUE(session_.close()->wait_for(WAIT_FOR_TIME));
    Unit::TearDown();
  }

  const cass::String& client_id() const { return client_id_; }
  cass::Config& config() { return config_; }
  const mockssandra::RequestHandler* simple_with_client_options() {
    mockssandra::SimpleRequestHandlerBuilder builder;
    builder.on(mockssandra::OPCODE_QUERY)
      .system_local()
      .system_peers()
      .client_options() // Allow for fake query to get client options
      .empty_rows_result(1);
    return builder.build();
  }

  void connect() {
    config_.contact_points().push_back("127.0.0.1");
    cass::Future::Ptr connect_future(session_.connect(config_));
    ASSERT_TRUE(connect_future->wait_for(WAIT_FOR_TIME)) << "Timed out waiting for session to connect";
    ASSERT_FALSE(connect_future->error())
      << cass_error_desc(connect_future->error()->code) << ": "
      << connect_future->error()->message;

    char client_id[CASS_UUID_STRING_LENGTH];
    cass_uuid_string(session_.client_id(), client_id);
    client_id_ = client_id;
  }

  cass::Map<cass::String, cass::String> client_options() {
    cass::SharedRefPtr<cass::QueryRequest> request(cass::Memory::allocate<cass::QueryRequest>(CLIENT_OPTIONS_QUERY, 0));
    cass::ResponseFuture::Ptr future = static_cast<cass::ResponseFuture::Ptr>(session_.execute(request, NULL));
    EXPECT_TRUE(future->wait_for(WAIT_FOR_TIME)) << "Timed out executing query";
    EXPECT_FALSE(future->error())
      << cass_error_desc(future->error()->code) << ": "
      << future->error()->message;

    cass::Map<cass::String, cass::String> options;
    cass::ResultResponse::Ptr response = static_cast<cass::ResultResponse::Ptr>(future->response());
    const cass::Row row = response->first_row();
    for (size_t i = 0; i < row.values.size(); ++i) {
      cass::String key = response->metadata()->get_column_definition(i).name.to_string();
      cass::String value = row.values[i].decoder().as_string();
      options[key] = value;
    }

    return options;
  }

private:
  cass::Config config_;
  cass::Session session_;
  cass::String client_id_;
};

TEST_F(StartupRequestUnitTest, Standard) {
  mockssandra::SimpleCluster cluster(simple_with_client_options());
  ASSERT_EQ(cluster.start_all(), 0);

  connect();
  cass::Map<cass::String, cass::String> options = client_options();
  ASSERT_EQ(4u, options.size());

  ASSERT_EQ(client_id(), options["CLIENT_ID"]);
  ASSERT_EQ(CASS_DEFAULT_CQL_VERSION, options["CQL_VERSION"]);
  ASSERT_EQ(cass::driver_name(), options["DRIVER_NAME"]);
  ASSERT_EQ(cass::driver_version(), options["DRIVER_VERSION"]);
}

TEST_F(StartupRequestUnitTest, EnableNoCompact) {
  mockssandra::SimpleCluster cluster(simple_with_client_options());
  ASSERT_EQ(cluster.start_all(), 0);

  config().set_no_compact(true);
  connect();
  cass::Map<cass::String, cass::String> options = client_options();
  ASSERT_EQ(5u, options.size());

  ASSERT_EQ(client_id(), options["CLIENT_ID"]);
  ASSERT_EQ(CASS_DEFAULT_CQL_VERSION, options["CQL_VERSION"]);
  ASSERT_EQ(cass::driver_name(), options["DRIVER_NAME"]);
  ASSERT_EQ(cass::driver_version(), options["DRIVER_VERSION"]);
  ASSERT_EQ("true", options["NO_COMPACT"]);
}

TEST_F(StartupRequestUnitTest, Application) {
  mockssandra::SimpleCluster cluster(simple_with_client_options());
  ASSERT_EQ(cluster.start_all(), 0);

  config().set_application_name(APPLICATION_NAME);
  config().set_application_version(APPLICATION_VERSION);
  connect();
  cass::Map<cass::String, cass::String> options = client_options();
  ASSERT_EQ(6u, options.size());

  ASSERT_EQ(APPLICATION_NAME, options["APPLICATION_NAME"]);
  ASSERT_EQ(APPLICATION_VERSION, options["APPLICATION_VERSION"]);
  ASSERT_EQ(client_id(), options["CLIENT_ID"]);
  ASSERT_EQ(CASS_DEFAULT_CQL_VERSION, options["CQL_VERSION"]);
  ASSERT_EQ(cass::driver_name(), options["DRIVER_NAME"]);
  ASSERT_EQ(cass::driver_version(), options["DRIVER_VERSION"]);
}
