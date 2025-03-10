/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "HermesInstance.h"

#include <hermes/inspector-modern/chrome/HermesRuntimeAgentDelegate.h>
#include <jsi/jsilib.h>
#include <jsinspector-modern/InspectorFlags.h>
#include <react/featureflags/ReactNativeFeatureFlags.h>

#ifdef HERMES_ENABLE_DEBUGGER
#include <hermes/inspector-modern/chrome/Registration.h>
#include <hermes/inspector/RuntimeAdapter.h>
#include <jsi/decorator.h>
#endif

using namespace facebook::hermes;
using namespace facebook::jsi;

namespace facebook::react {

#ifdef HERMES_ENABLE_DEBUGGER

// Wrapper that strongly retains the HermesRuntime for on device debugging.
//
// HermesInstanceRuntimeAdapter needs to strongly retain the HermesRuntime. Why:
//   - facebook::hermes::inspector_modern::chrome::Connection::Impl owns the
//   Adapter
//   - facebook::hermes::inspector_modern::chrome::Connection::Impl also owns
//   jsi:: objects
//   - jsi:: objects need to be deleted before the Runtime.
//
// If Adapter doesn't share ownership over jsi::Runtime, the runtime can be
// deleted before Connection::Impl cleans up all its jsi:: Objects. This will
// lead to a runtime crash.
class HermesInstanceRuntimeAdapter : public inspector_modern::RuntimeAdapter {
 public:
  HermesInstanceRuntimeAdapter(
      std::shared_ptr<HermesRuntime> hermesRuntime,
      std::shared_ptr<MessageQueueThread> msgQueueThread)
      : hermesRuntime_(std::move(hermesRuntime)),
        messageQueueThread_(std::move(msgQueueThread)) {}
  virtual ~HermesInstanceRuntimeAdapter() = default;

  HermesRuntime& getRuntime() override {
    return *hermesRuntime_;
  }

  void tickleJs() override {
    std::weak_ptr<HermesRuntime> weakRuntime(hermesRuntime_);
    messageQueueThread_->runOnQueue([weakRuntime]() {
      auto runtime = weakRuntime.lock();
      if (!runtime) {
        return;
      }
      jsi::Function func =
          runtime->global().getPropertyAsFunction(*runtime, "__tickleJs");
      func.call(*runtime);
    });
  }

 private:
  std::shared_ptr<HermesRuntime> hermesRuntime_;
  std::shared_ptr<MessageQueueThread> messageQueueThread_;
};

class DecoratedRuntime : public jsi::RuntimeDecorator<jsi::Runtime> {
 public:
  DecoratedRuntime(
      std::unique_ptr<HermesRuntime> runtime,
      std::shared_ptr<MessageQueueThread> msgQueueThread)
      : RuntimeDecorator<jsi::Runtime>(*runtime), runtime_(std::move(runtime)) {
    auto adapter = std::make_unique<HermesInstanceRuntimeAdapter>(
        runtime_, msgQueueThread);

    debugToken_ = inspector_modern::chrome::enableDebugging(
        std::move(adapter), "Hermes Bridgeless React Native");
  }

  ~DecoratedRuntime() {
    inspector_modern::chrome::disableDebugging(debugToken_);
  }

 private:
  std::shared_ptr<HermesRuntime> runtime_;
  inspector_modern::chrome::DebugSessionToken debugToken_;
};

#endif

class HermesJSRuntime : public JSRuntime {
 public:
  HermesJSRuntime(
      std::unique_ptr<HermesRuntime> runtime,
      std::shared_ptr<MessageQueueThread> msgQueueThread)
      : runtime_(std::move(runtime)),
        msgQueueThread_(std::move(msgQueueThread)) {}

  jsi::Runtime& getRuntime() noexcept override {
    return *runtime_;
  }

  std::unique_ptr<jsinspector_modern::RuntimeAgentDelegate> createAgentDelegate(
      jsinspector_modern::FrontendChannel frontendChannel,
      jsinspector_modern::SessionState& sessionState) override {
    return std::unique_ptr<jsinspector_modern::RuntimeAgentDelegate>(
        new jsinspector_modern::HermesRuntimeAgentDelegate(
            frontendChannel,
            sessionState,
            runtime_,
            [msgQueueThreadWeak = std::weak_ptr(msgQueueThread_),
             runtimeWeak = std::weak_ptr(runtime_)](auto fn) {
              auto msgQueueThread = msgQueueThreadWeak.lock();
              if (!msgQueueThread) {
                return;
              }
              msgQueueThread->runOnQueue([runtimeWeak, fn]() {
                auto runtime = runtimeWeak.lock();
                if (!runtime) {
                  return;
                }
                fn(*runtime);
              });
            }));
  }

 private:
  std::shared_ptr<HermesRuntime> runtime_;
  std::shared_ptr<MessageQueueThread> msgQueueThread_;
};

std::unique_ptr<JSRuntime> HermesInstance::createJSRuntime(
    std::shared_ptr<const ReactNativeConfig> reactNativeConfig,
    std::shared_ptr<::hermes::vm::CrashManager> cm,
    std::shared_ptr<MessageQueueThread> msgQueueThread) noexcept {
  assert(msgQueueThread != nullptr);
  int64_t vmExperimentFlags = reactNativeConfig
      ? reactNativeConfig->getInt64("ios_hermes:vm_experiment_flags")
      : 0;

  int64_t heapSizeConfig = reactNativeConfig
      ? reactNativeConfig->getInt64("ios_hermes:rn_heap_size_mb")
      : 0;
  // Default to 3GB if MobileConfigs is not available
  auto heapSizeMB = heapSizeConfig > 0
      ? static_cast<::hermes::vm::gcheapsize_t>(heapSizeConfig)
      : 3072;

  ::hermes::vm::RuntimeConfig::Builder runtimeConfigBuilder =
      ::hermes::vm::RuntimeConfig::Builder()
          .withGCConfig(::hermes::vm::GCConfig::Builder()
                            .withMaxHeapSize(heapSizeMB << 20)
                            .withName("RNBridgeless")
                            // For the next two arguments: avoid GC before TTI
                            // by initializing the runtime to allocate directly
                            // in the old generation, but revert to normal
                            // operation when we reach the (first) TTI point.
                            .withAllocInYoung(false)
                            .withRevertToYGAtTTI(true)
                            .build())
          .withES6Proxy(false)
          .withEnableSampleProfiling(true)
          .withMicrotaskQueue(ReactNativeFeatureFlags::enableMicrotasks())
          .withVMExperimentFlags(vmExperimentFlags);

  if (cm) {
    runtimeConfigBuilder.withCrashMgr(cm);
  }

  std::unique_ptr<HermesRuntime> hermesRuntime =
      hermes::makeHermesRuntime(runtimeConfigBuilder.build());

#ifdef HERMES_ENABLE_DEBUGGER
  auto& inspectorFlags = jsinspector_modern::InspectorFlags::getInstance();
  if (!inspectorFlags.getEnableModernCDPRegistry()) {
    std::unique_ptr<DecoratedRuntime> decoratedRuntime =
        std::make_unique<DecoratedRuntime>(
            std::move(hermesRuntime), msgQueueThread);
    return std::make_unique<JSIRuntimeHolder>(std::move(decoratedRuntime));
  }
#endif

  return std::make_unique<HermesJSRuntime>(
      std::move(hermesRuntime), std::move(msgQueueThread));
}

} // namespace facebook::react
