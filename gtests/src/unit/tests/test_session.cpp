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

#include "event_loop_test.hpp"
#include "query_request.hpp"
#include "session.hpp"

#define KEYSPACE "datastax"
#define NUM_THREADS 2 // Number of threads to execute queries using a session
#define OUTAGE_PLAN_DELAY 250 // Reduced delay to incorporate larger outage plan

class SessionUnitTest : public EventLoopTest {
public:
  SessionUnitTest()
    : EventLoopTest("SessionUnitTest") { }

  void populate_outage_plan(OutagePlan* outage_plan) {
    // Multiple rolling restarts
    for (int i = 1; i <= 9; ++i) {
      int node = i % 3;
      outage_plan->stop_node(node, OUTAGE_PLAN_DELAY);
      outage_plan->start_node(node, OUTAGE_PLAN_DELAY);
    }

    // Add/Remove entries from the "system" tables
    outage_plan->remove_node(2, OUTAGE_PLAN_DELAY);
    outage_plan->stop_node(1, OUTAGE_PLAN_DELAY);
    outage_plan->add_node(2, OUTAGE_PLAN_DELAY);
    outage_plan->start_node(1, OUTAGE_PLAN_DELAY);
    outage_plan->stop_node(3, OUTAGE_PLAN_DELAY);
    outage_plan->stop_node(1, OUTAGE_PLAN_DELAY);
  }

  void query_on_threads(cass::Session* session) {
    uv_thread_t threads[NUM_THREADS];
    for (int i = 0; i < NUM_THREADS; ++i) {
      ASSERT_EQ(0, uv_thread_create(&threads[i], query, session));
    }
    for (int i = 0; i < NUM_THREADS; ++i) {
      uv_thread_join(&threads[i]);
    }
  }

  static void connect(const cass::Config& config,
                      cass::Session* session,
                      uint64_t wait_for_time_us = WAIT_FOR_TIME) {
    cass::Future::Ptr connect_future(session->connect(config));
    ASSERT_TRUE(connect_future->wait_for(wait_for_time_us)) << "Timed out waiting for session to connect";
    ASSERT_FALSE(connect_future->error())
      << cass_error_desc(connect_future->error()->code) << ": "
      << connect_future->error()->message;
  }

  static void connect(cass::Session* session,
                      cass::SslContext* ssl_context = NULL,
                      uint64_t wait_for_time_us = WAIT_FOR_TIME,
                      size_t num_nodes = 3) {
    cass::Config config;
    config.set_reconnect_wait_time(100); // Faster reconnect time to handle cluster starts and stops
    for (size_t i = 1; i <= num_nodes; ++i) {
      cass::OStringStream ss;
      ss << "127.0.0." << i;
      config.contact_points().push_back(ss.str());
    }
    if (ssl_context) {
      config.set_ssl_context(ssl_context);
    }
    connect(config, session, wait_for_time_us);
  }

  static void close(cass::Session* session,
                    uint64_t wait_for_time_us = WAIT_FOR_TIME) {
    cass::Future::Ptr close_future(session->close());
    ASSERT_TRUE(close_future->wait_for(wait_for_time_us)) << "Timed out waiting for session to close";
    ASSERT_FALSE(close_future->error())
      << cass_error_desc(close_future->error()->code) << ": "
      << close_future->error()->message;
  }

  static void query(cass::Session* session) {
    cass::QueryRequest::Ptr request(cass::Memory::allocate<cass::QueryRequest>("blah", 0));
    request->set_is_idempotent(true);

    cass::Future::Ptr future = session->execute(request, NULL);
    ASSERT_TRUE(future->wait_for(WAIT_FOR_TIME)) << "Timed out executing query";
    ASSERT_FALSE(future->error())
      << cass_error_desc(future->error()->code) << ": "
      << future->error()->message;
  }

  // uv_thread_create
  static void query(void* arg) {
    cass::Session* session = static_cast<cass::Session*>(arg);
    query(session);
  }

  class HostEventFuture : public cass::Future {
  public:
    typedef SharedRefPtr<HostEventFuture> Ptr;

    enum Type {
      INVALID,
      START_NODE,
      STOP_NODE,
      ADD_NODE,
      REMOVE_NODE
    };

    typedef std::pair<Type, Address> Event;

    HostEventFuture()
      : cass::Future(cass::Future::FUTURE_TYPE_GENERIC) { }

    Type type() { return event_.first; }

    void set_event(Type type, const Address& host) {
      cass::ScopedMutex lock(&mutex_);
      if (!is_set()) {
        event_ = Event(type, host);
        internal_set(lock);
      }
    }

    Event wait_for_event(uint64_t timeout_us) {
      cass::ScopedMutex lock(&mutex_);
      return internal_wait_for(lock, timeout_us) ? event_ : Event(INVALID,
                                                                  Address());
    }

  private:
    Event event_;
  };

  class TestHostListener : public cass::DefaultHostListener {
  public:
    typedef cass::SharedRefPtr<TestHostListener> Ptr;

    TestHostListener() {
      events_.push_back(
        HostEventFuture::Ptr(
        Memory::allocate<HostEventFuture>()));
      uv_mutex_init(&mutex_);
    }

    ~TestHostListener() {
      uv_mutex_destroy(&mutex_);
    }

    virtual void on_host_up(const cass::Host::Ptr& host) {
      push_back(HostEventFuture::START_NODE, host);
    }

    virtual void on_host_down(const cass::Host::Ptr& host) {
      push_back(HostEventFuture::STOP_NODE, host);
    }

    virtual void on_host_added(const cass::Host::Ptr& host) {
      push_back(HostEventFuture::ADD_NODE, host);
    }

    virtual void on_host_removed(const cass::Host::Ptr& host) {
      push_back(HostEventFuture::REMOVE_NODE, host);
    }

    HostEventFuture::Event wait_for_event(uint64_t timeout_us) {
      HostEventFuture::Event event(front()->wait_for_event(timeout_us));
      pop_front();
      return event;
    }

    size_t event_count() {
      cass::ScopedMutex lock(&mutex_);
      size_t count = events_.size();
      return events_.front()->ready() ? count : count - 1;
    }

  private:
    typedef cass::Deque<HostEventFuture::Ptr> EventQueue;

    HostEventFuture::Ptr front() {
      cass::ScopedMutex lock(&mutex_);
      return events_.front();
    }

    void pop_front() {
      cass::ScopedMutex lock(&mutex_);
      events_.pop_front();
    }

    void push_back(HostEventFuture::Type type, const cass::Host::Ptr& host) {
      cass::ScopedMutex lock(&mutex_);
      events_.back()->set_event(type, host->address());
      events_.push_back(
        HostEventFuture::Ptr(
        Memory::allocate<HostEventFuture>()));
    }

  private:
    uv_mutex_t mutex_;
    EventQueue events_;
  };
};

TEST_F(SessionUnitTest, ExecuteQueryNotConnected) {
  cass::QueryRequest::Ptr request(cass::Memory::allocate<cass::QueryRequest>("blah", 0));

  cass::Session session;
  cass::Future::Ptr future = session.execute(request, NULL);
  ASSERT_EQ(CASS_ERROR_LIB_NO_HOSTS_AVAILABLE, future->error()->code);
}

TEST_F(SessionUnitTest, InvalidKeyspace) {
  mockssandra::SimpleRequestHandlerBuilder builder;
  builder.on(mockssandra::OPCODE_QUERY)
    .system_local()
    .system_peers()
    .use_keyspace("blah")
    .empty_rows_result(1);
  mockssandra::SimpleCluster cluster(builder.build());
  ASSERT_EQ(cluster.start_all(), 0);

  cass::Config config;
  config.contact_points().push_back("127.0.0.1");
  cass::Session session;

  cass::Future::Ptr connect_future(session.connect(config, "invalid"));
  ASSERT_TRUE(connect_future->wait_for(WAIT_FOR_TIME));
  ASSERT_EQ(CASS_ERROR_LIB_UNABLE_TO_SET_KEYSPACE, connect_future->error()->code);

  ASSERT_TRUE(session.close()->wait_for(WAIT_FOR_TIME));
}

TEST_F(SessionUnitTest, InvalidDataCenter) {
  mockssandra::SimpleCluster cluster(simple());
  ASSERT_EQ(cluster.start_all(), 0);

  cass::Config config;
  config.contact_points().push_back("127.0.0.1");
  config.set_load_balancing_policy(cass::Memory::allocate<cass::DCAwarePolicy>(
                                     "invalid_data_center",
                                     0,
                                     false));
  cass::Session session;

  cass::Future::Ptr connect_future(session.connect(config));
  ASSERT_TRUE(connect_future->wait_for(WAIT_FOR_TIME));
  ASSERT_EQ(CASS_ERROR_LIB_NO_HOSTS_AVAILABLE, connect_future->error()->code);

  ASSERT_TRUE(session.close()->wait_for(WAIT_FOR_TIME));
}


TEST_F(SessionUnitTest, InvalidLocalAddress) {
  mockssandra::SimpleCluster cluster(simple());
  ASSERT_EQ(cluster.start_all(), 0);

  cass::Config config;
  config.set_local_address(Address("1.1.1.1", PORT)); // Invalid
  config.contact_points().push_back("127.0.0.1");
  config.set_load_balancing_policy(cass::Memory::allocate<cass::DCAwarePolicy>(
                                     "invalid_data_center",
                                     0,
                                     false));
  cass::Session session;

  cass::Future::Ptr connect_future(session.connect(config, "invalid"));
  ASSERT_TRUE(connect_future->wait_for(WAIT_FOR_TIME));
  ASSERT_EQ(CASS_ERROR_LIB_NO_HOSTS_AVAILABLE, connect_future->error()->code);

  ASSERT_TRUE(session.close()->wait_for(WAIT_FOR_TIME));
}

TEST_F(SessionUnitTest, ExecuteQueryReusingSession) {
  mockssandra::SimpleCluster cluster(simple());
  ASSERT_EQ(cluster.start_all(), 0);

  cass::Session session;
  for (int i = 0; i < 2; ++i) {
    connect(&session);
    query(&session);
    close(&session);
  }
}

TEST_F(SessionUnitTest, ExecuteQueryReusingSessionUsingSsl) {
  mockssandra::SimpleCluster cluster(simple());
  cass::SslContext::Ptr ssl_context = use_ssl(&cluster).socket_settings.ssl_context;
  ASSERT_EQ(cluster.start_all(), 0);

  cass::Session session;
  for (int i = 0; i < 2; ++i) {
    connect(&session, ssl_context.get());
    query(&session);
    close(&session);
  }
}

TEST_F(SessionUnitTest, ExecuteQueryReusingSessionChaotic) {
  mockssandra::SimpleCluster cluster(simple(), 4);
  ASSERT_EQ(cluster.start_all(), 0);

  OutagePlan outage_plan(loop(), &cluster);
  populate_outage_plan(&outage_plan);

  cass::Session session;
  cass::Future::Ptr outage_future = execute_outage_plan(&outage_plan);
  while (!outage_future->wait_for(1000)) { // 1 millisecond wait
    connect(&session, NULL, WAIT_FOR_TIME * 3, 4);
    query(&session);
    close(&session, WAIT_FOR_TIME * 3);
  }
}

TEST_F(SessionUnitTest, ExecuteQueryReusingSessionUsingSslChaotic) {
  mockssandra::SimpleCluster cluster(simple(), 4);
  cass::SslContext::Ptr ssl_context = use_ssl(&cluster).socket_settings.ssl_context;
  ASSERT_EQ(cluster.start_all(), 0);

  OutagePlan outage_plan(loop(), &cluster);
  populate_outage_plan(&outage_plan);

  cass::Session session;
  cass::Future::Ptr outage_future = execute_outage_plan(&outage_plan);
  while (!outage_future->wait_for(1000)) { // 1 millisecond wait
    connect(&session, ssl_context.get(), WAIT_FOR_TIME * 3, 4);
    query(&session);
    close(&session, WAIT_FOR_TIME * 3);
  }
}

TEST_F(SessionUnitTest, ExecuteQueryWithCompleteOutage) {
  mockssandra::SimpleCluster cluster(simple(), 3);
  ASSERT_EQ(cluster.start_all(), 0);

  cass::Session session;
  connect(&session);

  // Full outage
  cluster.stop_all();
  cass::QueryRequest::Ptr request(cass::Memory::allocate<cass::QueryRequest>("blah", 0));
  cass::Future::Ptr future = session.execute(request, NULL);
  ASSERT_TRUE(future->wait_for(WAIT_FOR_TIME));
  ASSERT_TRUE(future->error());
  EXPECT_TRUE(CASS_ERROR_LIB_NO_HOSTS_AVAILABLE == future->error()->code ||
              CASS_ERROR_LIB_REQUEST_TIMED_OUT == future->error()->code);

  // Restart a node and execute query to ensure session recovers
  ASSERT_EQ(cluster.start(2), 0);
  test::Utils::msleep(200); // Give time for the reconnect to start
  query(&session);

  close(&session);
}

TEST_F(SessionUnitTest, ExecuteQueryWithCompleteOutageSpinDown) {
  mockssandra::SimpleCluster cluster(simple(), 3);
  ASSERT_EQ(cluster.start_all(), 0);

  cass::Session session;
  connect(&session);

  // Spin down nodes while querying
  query(&session);
  cluster.stop(3);
  query(&session);
  cluster.stop(1);
  query(&session);
  cluster.stop(2);

  // Full outage
  cass::QueryRequest::Ptr request(cass::Memory::allocate<cass::QueryRequest>("blah", 0));
  cass::Future::Ptr future = session.execute(request, NULL);
  ASSERT_TRUE(future->wait_for(WAIT_FOR_TIME));
  ASSERT_EQ(CASS_ERROR_LIB_NO_HOSTS_AVAILABLE, future->error()->code);

  // Restart a node and execute query to ensure session recovers
  ASSERT_EQ(cluster.start(2), 0);
  test::Utils::msleep(200); // Give time for the reconnect to start
  query(&session);

  close(&session);
}

TEST_F(SessionUnitTest, ExecuteQueryWithThreads) {
  mockssandra::SimpleCluster cluster(simple());
  ASSERT_EQ(cluster.start_all(), 0);

  cass::Session session;
  connect(&session);
  query_on_threads(&session);
  close(&session);
}

TEST_F(SessionUnitTest, ExecuteQueryWithThreadsUsingSsl) {
  mockssandra::SimpleCluster cluster(simple());
  cass::SslContext::Ptr ssl_context = use_ssl(&cluster).socket_settings.ssl_context;
  ASSERT_EQ(cluster.start_all(), 0);

  cass::Session session;
  connect(&session, ssl_context.get());
  query_on_threads(&session);
  close(&session);
}

TEST_F(SessionUnitTest, ExecuteQueryWithThreadsChaotic) {
  mockssandra::SimpleCluster cluster(simple(), 4);
  ASSERT_EQ(cluster.start_all(), 0);

  cass::Session session;
  connect(&session);

  OutagePlan outage_plan(loop(), &cluster);
  populate_outage_plan(&outage_plan);

  cass::Future::Ptr outage_future = execute_outage_plan(&outage_plan);
  while (!outage_future->wait_for(1000)) { // 1 millisecond wait
    query_on_threads(&session);
  }

  close(&session);
}

TEST_F(SessionUnitTest, ExecuteQueryWithThreadsUsingSslChaotic) {
  mockssandra::SimpleCluster cluster(simple(), 4);
  cass::SslContext::Ptr ssl_context = use_ssl(&cluster).socket_settings.ssl_context;
  ASSERT_EQ(cluster.start_all(), 0);

  cass::Session session;
  connect(&session, ssl_context.get());

  OutagePlan outage_plan(loop(), &cluster);
  populate_outage_plan(&outage_plan);

  cass::Future::Ptr outage_future = execute_outage_plan(&outage_plan);
  while (!outage_future->wait_for(1000)) { // 1 millisecond wait
    query_on_threads(&session);
  }

  close(&session);
}

TEST_F(SessionUnitTest, HostListener) {
  mockssandra::SimpleCluster cluster(simple(), 2);
  ASSERT_EQ(cluster.start_all(), 0);

  TestHostListener::Ptr listener(cass::Memory::allocate<TestHostListener>());

  cass::Config config;
  config.set_reconnect_wait_time(100); // Reconnect immediately
  config.contact_points().push_back("127.0.0.2");
  config.set_host_listener(listener);

  cass::Session session;
  connect(config, &session);

  EXPECT_EQ(0u, listener->event_count());

  {
    cluster.remove(1);
    EXPECT_EQ(HostEventFuture::Event(HostEventFuture::STOP_NODE,
                                     Address("127.0.0.1", 9042)),
              listener->wait_for_event(WAIT_FOR_TIME));
    EXPECT_EQ(HostEventFuture::Event(HostEventFuture::REMOVE_NODE,
                                     Address("127.0.0.1", 9042)),
              listener->wait_for_event(WAIT_FOR_TIME));
  }

  {
    cluster.add(1);
    EXPECT_EQ(HostEventFuture::Event(HostEventFuture::ADD_NODE,
                                     Address("127.0.0.1", 9042)),
              listener->wait_for_event(WAIT_FOR_TIME));
    EXPECT_EQ(HostEventFuture::Event(HostEventFuture::START_NODE,
                                     Address("127.0.0.1", 9042)),
              listener->wait_for_event(WAIT_FOR_TIME));
  }

  {
    cluster.stop(2);
    EXPECT_EQ(HostEventFuture::Event(HostEventFuture::STOP_NODE,
                                     Address("127.0.0.2", 9042)),
              listener->wait_for_event(WAIT_FOR_TIME));
  }

  {
    cluster.start(2);
    EXPECT_EQ(HostEventFuture::Event(HostEventFuture::START_NODE,
                                     Address("127.0.0.2", 9042)),
              listener->wait_for_event(WAIT_FOR_TIME));
  }

  close(&session);

  ASSERT_EQ(0u, listener->event_count());
}
