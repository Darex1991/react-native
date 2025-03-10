/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <folly/executors/QueuedImmediateExecutor.h>
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <jsinspector-modern/InspectorInterfaces.h>
#include <jsinspector-modern/PageTarget.h>

#include <memory>

#include "FollyDynamicMatchers.h"
#include "InspectorMocks.h"
#include "UniquePtrFactory.h"
#include "engines/JsiIntegrationTestGenericEngineAdapter.h"
#include "engines/JsiIntegrationTestHermesEngineAdapter.h"

using namespace ::testing;

namespace facebook::react::jsinspector_modern {

namespace {

/**
 * A text fixture class for the integration between the modern RN CDP backend
 * and a JSI engine, mocking out the rest of RN. For simplicity, everything is
 * single-threaded and "async" work is actually done through a queued immediate
 * executor ( = run immediately and finish all queued sub-tasks before
 * returning).
 *
 * The main limitation of the simpler threading model is that we can't cover
 * breakpoints etc - since pausing during JS execution would prevent the test
 * from making progress. Such functionality is better suited for a full RN+CDP
 * integration test (using RN's own thread management) as well as for each
 * engine's unit tests.
 *
 * \tparam EngineAdapter An adapter class that implements RuntimeTargetDelegate
 * for a particular engine, plus exposes access to a RuntimeExecutor (based on
 * the provided folly::Executor) and the corresponding jsi::Runtime.
 */
template <typename EngineAdapter>
class JsiIntegrationPortableTest : public Test, private PageTargetDelegate {
  folly::QueuedImmediateExecutor immediateExecutor_;

 protected:
  JsiIntegrationPortableTest() : engineAdapter_{immediateExecutor_} {
    instance_ = &page_->registerInstance(instanceTargetDelegate_);
    runtimeTarget_ = &instance_->registerRuntime(
        *engineAdapter_, engineAdapter_->getRuntimeExecutor());
  }

  ~JsiIntegrationPortableTest() override {
    toPage_.reset();
    if (runtimeTarget_) {
      EXPECT_TRUE(instance_);
      instance_->unregisterRuntime(*runtimeTarget_);
      runtimeTarget_ = nullptr;
    }
    if (instance_) {
      page_->unregisterInstance(*instance_);
      instance_ = nullptr;
    }
  }

  void connect() {
    ASSERT_FALSE(toPage_) << "Can only connect once in a JSI integration test.";
    toPage_ = page_->connect(
        remoteConnections_.make_unique(),
        {.integrationName = "JsiIntegrationTest"});

    // We'll always get an onDisconnect call when we tear
    // down the test. Expect it in order to satisfy the strict mock.
    EXPECT_CALL(*remoteConnections_[0], onDisconnect());
  }

  void reload() {
    if (runtimeTarget_) {
      ASSERT_TRUE(instance_);
      instance_->unregisterRuntime(*runtimeTarget_);
      runtimeTarget_ = nullptr;
    }
    if (instance_) {
      page_->unregisterInstance(*instance_);
      instance_ = nullptr;
    }
    // Recreate the engine (e.g. to wipe any state in the inner jsi::Runtime)
    engineAdapter_.emplace(immediateExecutor_);
    instance_ = &page_->registerInstance(instanceTargetDelegate_);
    runtimeTarget_ = &instance_->registerRuntime(
        *engineAdapter_, engineAdapter_->getRuntimeExecutor());
  }

  MockRemoteConnection& fromPage() {
    assert(toPage_);
    return *remoteConnections_[0];
  }

  VoidExecutor inspectorExecutor_ = [this](auto callback) {
    immediateExecutor_.add(callback);
  };

  jsi::Value eval(std::string_view code) {
    return engineAdapter_->getRuntime().evaluateJavaScript(
        std::make_shared<jsi::StringBuffer>(std::string(code)), "<eval>");
  }

  std::shared_ptr<PageTarget> page_ =
      PageTarget::create(*this, inspectorExecutor_);
  InstanceTarget* instance_{};
  RuntimeTarget* runtimeTarget_{};

  MockInstanceTargetDelegate instanceTargetDelegate_;
  std::optional<EngineAdapter> engineAdapter_;

 private:
  UniquePtrFactory<StrictMock<MockRemoteConnection>> remoteConnections_;

 protected:
  // NOTE: Needs to be destroyed before page_.
  std::unique_ptr<ILocalConnection> toPage_;

 private:
  // PageTargetDelegate methods

  void onReload(const PageReloadRequest& request) override {
    (void)request;
    reload();
  }
};

} // namespace

////////////////////////////////////////////////////////////////////////////////

// Some tests are specific to Hermes's CDP capabilities and some are not.
// We'll use JsiIntegrationHermesTest as a fixture for Hermes-specific tests
// and typed tests for the engine-agnostic ones.

using JsiIntegrationHermesTest =
    JsiIntegrationPortableTest<JsiIntegrationTestHermesEngineAdapter>;

/**
 * The list of engine adapters for which engine-agnostic tests should pass.
 */
using AllEngines = Types<
    JsiIntegrationTestHermesEngineAdapter,
    JsiIntegrationTestGenericEngineAdapter>;

TYPED_TEST_SUITE(JsiIntegrationPortableTest, AllEngines);

////////////////////////////////////////////////////////////////////////////////

TYPED_TEST(JsiIntegrationPortableTest, ConnectWithoutCrashing) {
  this->connect();
}

TYPED_TEST(JsiIntegrationPortableTest, ErrorOnUnknownMethod) {
  this->connect();

  EXPECT_CALL(
      this->fromPage(),
      onMessage(JsonParsed(
          AllOf(AtJsonPtr("/id", 1), AtJsonPtr("/error/code", -32601)))))
      .RetiresOnSaturation();

  this->toPage_->sendMessage(R"({
                                 "id": 1,
                                 "method": "Foobar.unknownMethod"
                               })");
}

////////////////////////////////////////////////////////////////////////////////

TEST_F(JsiIntegrationHermesTest, EvaluateExpression) {
  connect();

  EXPECT_CALL(fromPage(), onMessage(JsonEq(R"({
                                             "id": 1,
                                             "result": {
                                               "result": {
                                                 "type": "number",
                                                 "value": 42
                                               }
                                              }
                                            })")));
  toPage_->sendMessage(R"({
                           "id": 1,
                           "method": "Runtime.evaluate",
                           "params": {"expression": "42"}
                         })");
}

TEST_F(JsiIntegrationHermesTest, ExecutionContextNotifications) {
  connect();

  InSequence s;

  // NOTE: This is the wrong sequence of responses from Hermes - the
  // notification should come before the method response.
  EXPECT_CALL(fromPage(), onMessage(JsonEq(R"({
                                             "id": 1,
                                             "result": {}
                                            })")));
  EXPECT_CALL(
      fromPage(),
      onMessage(JsonParsed(
          AllOf(AtJsonPtr("/method", Eq("Runtime.executionContextCreated"))))))
      .RetiresOnSaturation();

  toPage_->sendMessage(R"({
                           "id": 1,
                           "method": "Runtime.enable"
                         })");

  // NOTE: Missing a Runtime.executionContextDestroyed notification here.

  EXPECT_CALL(fromPage(), onMessage(JsonEq(R"({
                                               "method": "Runtime.executionContextsCleared"
                                             })")))
      .RetiresOnSaturation();
  EXPECT_CALL(
      fromPage(),
      onMessage(JsonParsed(
          AllOf(AtJsonPtr("/method", Eq("Runtime.executionContextCreated"))))))
      .RetiresOnSaturation();
  // Simulate a reload triggered by the app (not by the debugger).
  reload();

  // NOTE: Missing a Runtime.executionContextDestroyed notification here.

  EXPECT_CALL(fromPage(), onMessage(JsonEq(R"({
                                               "method": "Runtime.executionContextsCleared"
                                             })")))
      .RetiresOnSaturation();
  EXPECT_CALL(
      fromPage(),
      onMessage(JsonParsed(
          AllOf(AtJsonPtr("/method", Eq("Runtime.executionContextCreated"))))))
      .RetiresOnSaturation();
  EXPECT_CALL(fromPage(), onMessage(JsonEq(R"({
                                               "id": 2,
                                               "result": {}
                                             })")))
      .RetiresOnSaturation();
  toPage_->sendMessage(R"({
                           "id": 2,
                           "method": "Page.reload"
                         })");
}

} // namespace facebook::react::jsinspector_modern
