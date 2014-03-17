// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <algorithm>
#include <string>
#include <vector>

#include "base/bind.h"
#include "base/memory/scoped_ptr.h"
#include "base/message_loop/message_loop.h"
#include "base/stl_util.h"
#include "base/test/test_timeouts.h"
#include "base/threading/thread.h"
#include "base/threading/thread_restrictions.h"
#include "dbus/bus.h"
#include "dbus/message.h"
#include "dbus/object_path.h"
#include "dbus/object_proxy.h"
#include "dbus/test_service.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace dbus {

namespace {

// See comments in ObjectProxy::RunResponseCallback() for why the number was
// chosen.
const int kHugePayloadSize = 64 << 20;  // 64 MB

}  // namespace

// The end-to-end test exercises the asynchronous APIs in ObjectProxy and
// ExportedObject.
class EndToEndAsyncTest : public testing::Test {
 public:
  EndToEndAsyncTest() : on_disconnected_call_count_(0) {}

  virtual void SetUp() {
    // Make the main thread not to allow IO.
    base::ThreadRestrictions::SetIOAllowed(false);

    // Start the D-Bus thread.
    dbus_thread_.reset(new base::Thread("D-Bus Thread"));
    base::Thread::Options thread_options;
    thread_options.message_loop_type = base::MessageLoop::TYPE_IO;
    ASSERT_TRUE(dbus_thread_->StartWithOptions(thread_options));

    // Start the test service, using the D-Bus thread.
    TestService::Options options;
    options.dbus_task_runner = dbus_thread_->message_loop_proxy();
    test_service_.reset(new TestService(options));
    ASSERT_TRUE(test_service_->StartService());
    ASSERT_TRUE(test_service_->WaitUntilServiceIsStarted());
    ASSERT_TRUE(test_service_->HasDBusThread());

    // Create the client, using the D-Bus thread.
    Bus::Options bus_options;
    bus_options.bus_type = Bus::SESSION;
    bus_options.connection_type = Bus::PRIVATE;
    bus_options.dbus_task_runner = dbus_thread_->message_loop_proxy();
    bus_options.disconnected_callback =
        base::Bind(&EndToEndAsyncTest::OnDisconnected, base::Unretained(this));
    bus_ = new Bus(bus_options);
    object_proxy_ = bus_->GetObjectProxy(
        "org.chromium.TestService",
        ObjectPath("/org/chromium/TestObject"));
    ASSERT_TRUE(bus_->HasDBusThread());

    // Connect to the "Test" signal of "org.chromium.TestInterface" from
    // the remote object.
    object_proxy_->ConnectToSignal(
        "org.chromium.TestInterface",
        "Test",
        base::Bind(&EndToEndAsyncTest::OnTestSignal,
                   base::Unretained(this)),
        base::Bind(&EndToEndAsyncTest::OnConnected,
                   base::Unretained(this)));
    // Wait until the object proxy is connected to the signal.
    message_loop_.Run();

    // Connect to the "Test2" signal of "org.chromium.TestInterface" from
    // the remote object. There was a bug where we were emitting error
    // messages like "Requested to remove an unknown match rule: ..." at
    // the shutdown of Bus when an object proxy is connected to more than
    // one signal of the same interface. See crosbug.com/23382 for details.
    object_proxy_->ConnectToSignal(
        "org.chromium.TestInterface",
        "Test2",
        base::Bind(&EndToEndAsyncTest::OnTest2Signal,
                   base::Unretained(this)),
        base::Bind(&EndToEndAsyncTest::OnConnected,
                   base::Unretained(this)));
    // Wait until the object proxy is connected to the signal.
    message_loop_.Run();

    // Create a second object proxy for the root object.
    root_object_proxy_ = bus_->GetObjectProxy("org.chromium.TestService",
                                              ObjectPath("/"));
    ASSERT_TRUE(bus_->HasDBusThread());

    // Connect to the "Test" signal of "org.chromium.TestInterface" from
    // the root remote object too.
    root_object_proxy_->ConnectToSignal(
        "org.chromium.TestInterface",
        "Test",
        base::Bind(&EndToEndAsyncTest::OnRootTestSignal,
                   base::Unretained(this)),
        base::Bind(&EndToEndAsyncTest::OnConnected,
                   base::Unretained(this)));
    // Wait until the root object proxy is connected to the signal.
    message_loop_.Run();
  }

  virtual void TearDown() {
    bus_->ShutdownOnDBusThreadAndBlock();

    // Shut down the service.
    test_service_->ShutdownAndBlock();

    // Reset to the default.
    base::ThreadRestrictions::SetIOAllowed(true);

    // Stopping a thread is considered an IO operation, so do this after
    // allowing IO.
    test_service_->Stop();
  }

 protected:
  // Replaces the bus with a broken one.
  void SetUpBrokenBus() {
    // Shut down the existing bus.
    bus_->ShutdownOnDBusThreadAndBlock();

    // Create new bus with invalid address.
    const char kInvalidAddress[] = "";
    Bus::Options bus_options;
    bus_options.bus_type = Bus::CUSTOM_ADDRESS;
    bus_options.address = kInvalidAddress;
    bus_options.connection_type = Bus::PRIVATE;
    bus_options.dbus_task_runner = dbus_thread_->message_loop_proxy();
    bus_ = new Bus(bus_options);
    ASSERT_TRUE(bus_->HasDBusThread());

    // Create new object proxy.
    object_proxy_ = bus_->GetObjectProxy(
        "org.chromium.TestService",
        ObjectPath("/org/chromium/TestObject"));
  }

  // Calls the method asynchronously. OnResponse() will be called once the
  // response is received.
  void CallMethod(MethodCall* method_call,
                  int timeout_ms) {
    object_proxy_->CallMethod(method_call,
                              timeout_ms,
                              base::Bind(&EndToEndAsyncTest::OnResponse,
                                         base::Unretained(this)));
  }

  // Calls the method asynchronously. OnResponse() will be called once the
  // response is received without error, otherwise OnError() will be called.
  void CallMethodWithErrorCallback(MethodCall* method_call,
                                   int timeout_ms) {
    object_proxy_->CallMethodWithErrorCallback(
        method_call,
        timeout_ms,
        base::Bind(&EndToEndAsyncTest::OnResponse, base::Unretained(this)),
        base::Bind(&EndToEndAsyncTest::OnError, base::Unretained(this)));
  }

  // Wait for the give number of responses.
  void WaitForResponses(size_t num_responses) {
    while (response_strings_.size() < num_responses) {
      message_loop_.Run();
    }
  }

  // Called when the response is received.
  void OnResponse(Response* response) {
    // |response| will be deleted on exit of the function. Copy the
    // payload to |response_strings_|.
    if (response) {
      MessageReader reader(response);
      std::string response_string;
      ASSERT_TRUE(reader.PopString(&response_string));
      response_strings_.push_back(response_string);
    } else {
      response_strings_.push_back(std::string());
    }
    message_loop_.Quit();
  };

  // Wait for the given number of errors.
  void WaitForErrors(size_t num_errors) {
    while (error_names_.size() < num_errors) {
      message_loop_.Run();
    }
  }

  // Called when an error is received.
  void OnError(ErrorResponse* error) {
    // |error| will be deleted on exit of the function. Copy the payload to
    // |error_names_|.
    if (error) {
      ASSERT_NE("", error->GetErrorName());
      error_names_.push_back(error->GetErrorName());
    } else {
      error_names_.push_back(std::string());
    }
    message_loop_.Quit();
  }

  // Called when the "Test" signal is received, in the main thread.
  // Copy the string payload to |test_signal_string_|.
  void OnTestSignal(Signal* signal) {
    MessageReader reader(signal);
    ASSERT_TRUE(reader.PopString(&test_signal_string_));
    message_loop_.Quit();
  }

  // Called when the "Test" signal is received, in the main thread, by
  // the root object proxy. Copy the string payload to
  // |root_test_signal_string_|.
  void OnRootTestSignal(Signal* signal) {
    MessageReader reader(signal);
    ASSERT_TRUE(reader.PopString(&root_test_signal_string_));
    message_loop_.Quit();
  }

  // Called when the "Test2" signal is received, in the main thread.
  void OnTest2Signal(Signal* signal) {
    MessageReader reader(signal);
    message_loop_.Quit();
  }

  // Called when connected to the signal.
  void OnConnected(const std::string& interface_name,
                   const std::string& signal_name,
                   bool success) {
    ASSERT_TRUE(success);
    message_loop_.Quit();
  }

  // Called when the connection with dbus-daemon is disconnected.
  void OnDisconnected() {
    message_loop_.Quit();
    ++on_disconnected_call_count_;
  }

  // Wait for the hey signal to be received.
  void WaitForTestSignal() {
    // OnTestSignal() will quit the message loop.
    message_loop_.Run();
  }

  base::MessageLoop message_loop_;
  std::vector<std::string> response_strings_;
  std::vector<std::string> error_names_;
  scoped_ptr<base::Thread> dbus_thread_;
  scoped_refptr<Bus> bus_;
  ObjectProxy* object_proxy_;
  ObjectProxy* root_object_proxy_;
  scoped_ptr<TestService> test_service_;
  // Text message from "Test" signal.
  std::string test_signal_string_;
  // Text message from "Test" signal delivered to root.
  std::string root_test_signal_string_;
  int on_disconnected_call_count_;
};

TEST_F(EndToEndAsyncTest, Echo) {
  const char* kHello = "hello";

  // Create the method call.
  MethodCall method_call("org.chromium.TestInterface", "Echo");
  MessageWriter writer(&method_call);
  writer.AppendString(kHello);

  // Call the method.
  const int timeout_ms = ObjectProxy::TIMEOUT_USE_DEFAULT;
  CallMethod(&method_call, timeout_ms);

  // Check the response.
  WaitForResponses(1);
  EXPECT_EQ(kHello, response_strings_[0]);
}

TEST_F(EndToEndAsyncTest, EchoWithErrorCallback) {
  const char* kHello = "hello";

  // Create the method call.
  MethodCall method_call("org.chromium.TestInterface", "Echo");
  MessageWriter writer(&method_call);
  writer.AppendString(kHello);

  // Call the method.
  const int timeout_ms = ObjectProxy::TIMEOUT_USE_DEFAULT;
  CallMethodWithErrorCallback(&method_call, timeout_ms);

  // Check the response.
  WaitForResponses(1);
  EXPECT_EQ(kHello, response_strings_[0]);
  EXPECT_TRUE(error_names_.empty());
}

// Call Echo method three times.
TEST_F(EndToEndAsyncTest, EchoThreeTimes) {
  const char* kMessages[] = { "foo", "bar", "baz" };

  for (size_t i = 0; i < arraysize(kMessages); ++i) {
    // Create the method call.
    MethodCall method_call("org.chromium.TestInterface", "Echo");
    MessageWriter writer(&method_call);
    writer.AppendString(kMessages[i]);

    // Call the method.
    const int timeout_ms = ObjectProxy::TIMEOUT_USE_DEFAULT;
    CallMethod(&method_call, timeout_ms);
  }

  // Check the responses.
  WaitForResponses(3);
  // Sort as the order of the returned messages is not deterministic.
  std::sort(response_strings_.begin(), response_strings_.end());
  EXPECT_EQ("bar", response_strings_[0]);
  EXPECT_EQ("baz", response_strings_[1]);
  EXPECT_EQ("foo", response_strings_[2]);
}

TEST_F(EndToEndAsyncTest, Echo_HugePayload) {
  const std::string kHugePayload(kHugePayloadSize, 'o');

  // Create the method call with a huge payload.
  MethodCall method_call("org.chromium.TestInterface", "Echo");
  MessageWriter writer(&method_call);
  writer.AppendString(kHugePayload);

  // Call the method.
  const int timeout_ms = ObjectProxy::TIMEOUT_USE_DEFAULT;
  CallMethod(&method_call, timeout_ms);

  // This caused a DCHECK failure before. Ensure that the issue is fixed.
  WaitForResponses(1);
  EXPECT_EQ(kHugePayload, response_strings_[0]);
}

TEST_F(EndToEndAsyncTest, BrokenBus) {
  const char* kHello = "hello";

  // Set up a broken bus.
  SetUpBrokenBus();

  // Create the method call.
  MethodCall method_call("org.chromium.TestInterface", "Echo");
  MessageWriter writer(&method_call);
  writer.AppendString(kHello);

  // Call the method.
  const int timeout_ms = ObjectProxy::TIMEOUT_USE_DEFAULT;
  CallMethod(&method_call, timeout_ms);
  WaitForResponses(1);

  // Should fail because of the broken bus.
  ASSERT_EQ("", response_strings_[0]);
}

TEST_F(EndToEndAsyncTest, BrokenBusWithErrorCallback) {
  const char* kHello = "hello";

  // Set up a broken bus.
  SetUpBrokenBus();

  // Create the method call.
  MethodCall method_call("org.chromium.TestInterface", "Echo");
  MessageWriter writer(&method_call);
  writer.AppendString(kHello);

  // Call the method.
  const int timeout_ms = ObjectProxy::TIMEOUT_USE_DEFAULT;
  CallMethodWithErrorCallback(&method_call, timeout_ms);
  WaitForErrors(1);

  // Should fail because of the broken bus.
  ASSERT_TRUE(response_strings_.empty());
  ASSERT_EQ("", error_names_[0]);
}

TEST_F(EndToEndAsyncTest, Timeout) {
  const char* kHello = "hello";

  // Create the method call.
  MethodCall method_call("org.chromium.TestInterface", "SlowEcho");
  MessageWriter writer(&method_call);
  writer.AppendString(kHello);

  // Call the method with timeout of 0ms.
  const int timeout_ms = 0;
  CallMethod(&method_call, timeout_ms);
  WaitForResponses(1);

  // Should fail because of timeout.
  ASSERT_EQ("", response_strings_[0]);
}

TEST_F(EndToEndAsyncTest, TimeoutWithErrorCallback) {
  const char* kHello = "hello";

  // Create the method call.
  MethodCall method_call("org.chromium.TestInterface", "SlowEcho");
  MessageWriter writer(&method_call);
  writer.AppendString(kHello);

  // Call the method with timeout of 0ms.
  const int timeout_ms = 0;
  CallMethodWithErrorCallback(&method_call, timeout_ms);
  WaitForErrors(1);

  // Should fail because of timeout.
  ASSERT_TRUE(response_strings_.empty());
  ASSERT_EQ(DBUS_ERROR_NO_REPLY, error_names_[0]);
}

// Tests calling a method that sends its reply asynchronously.
TEST_F(EndToEndAsyncTest, AsyncEcho) {
  const char* kHello = "hello";

  // Create the method call.
  MethodCall method_call("org.chromium.TestInterface", "AsyncEcho");
  MessageWriter writer(&method_call);
  writer.AppendString(kHello);

  // Call the method.
  const int timeout_ms = ObjectProxy::TIMEOUT_USE_DEFAULT;
  CallMethod(&method_call, timeout_ms);

  // Check the response.
  WaitForResponses(1);
  EXPECT_EQ(kHello, response_strings_[0]);
}

TEST_F(EndToEndAsyncTest, NonexistentMethod) {
  MethodCall method_call("org.chromium.TestInterface", "Nonexistent");

  const int timeout_ms = ObjectProxy::TIMEOUT_USE_DEFAULT;
  CallMethod(&method_call, timeout_ms);
  WaitForResponses(1);

  // Should fail because the method is nonexistent.
  ASSERT_EQ("", response_strings_[0]);
}

TEST_F(EndToEndAsyncTest, NonexistentMethodWithErrorCallback) {
  MethodCall method_call("org.chromium.TestInterface", "Nonexistent");

  const int timeout_ms = ObjectProxy::TIMEOUT_USE_DEFAULT;
  CallMethodWithErrorCallback(&method_call, timeout_ms);
  WaitForErrors(1);

  // Should fail because the method is nonexistent.
  ASSERT_TRUE(response_strings_.empty());
  ASSERT_EQ(DBUS_ERROR_UNKNOWN_METHOD, error_names_[0]);
}

TEST_F(EndToEndAsyncTest, BrokenMethod) {
  MethodCall method_call("org.chromium.TestInterface", "BrokenMethod");

  const int timeout_ms = ObjectProxy::TIMEOUT_USE_DEFAULT;
  CallMethod(&method_call, timeout_ms);
  WaitForResponses(1);

  // Should fail because the method is broken.
  ASSERT_EQ("", response_strings_[0]);
}

TEST_F(EndToEndAsyncTest, BrokenMethodWithErrorCallback) {
  MethodCall method_call("org.chromium.TestInterface", "BrokenMethod");

  const int timeout_ms = ObjectProxy::TIMEOUT_USE_DEFAULT;
  CallMethodWithErrorCallback(&method_call, timeout_ms);
  WaitForErrors(1);

  // Should fail because the method is broken.
  ASSERT_TRUE(response_strings_.empty());
  ASSERT_EQ(DBUS_ERROR_FAILED, error_names_[0]);
}

TEST_F(EndToEndAsyncTest, InvalidObjectPath) {
  // Trailing '/' is only allowed for the root path.
  const ObjectPath invalid_object_path("/org/chromium/TestObject/");

  // Replace object proxy with new one.
  object_proxy_ = bus_->GetObjectProxy("org.chromium.TestService",
                                       invalid_object_path);

  MethodCall method_call("org.chromium.TestInterface", "Echo");

  const int timeout_ms = ObjectProxy::TIMEOUT_USE_DEFAULT;
  CallMethodWithErrorCallback(&method_call, timeout_ms);
  WaitForErrors(1);

  // Should fail because of the invalid path.
  ASSERT_TRUE(response_strings_.empty());
  ASSERT_EQ("", error_names_[0]);
}

TEST_F(EndToEndAsyncTest, InvalidServiceName) {
  // Bus name cannot contain '/'.
  const std::string invalid_service_name = ":1/2";

  // Replace object proxy with new one.
  object_proxy_ = bus_->GetObjectProxy(
      invalid_service_name, ObjectPath("org.chromium.TestObject"));

  MethodCall method_call("org.chromium.TestInterface", "Echo");

  const int timeout_ms = ObjectProxy::TIMEOUT_USE_DEFAULT;
  CallMethodWithErrorCallback(&method_call, timeout_ms);
  WaitForErrors(1);

  // Should fail because of the invalid bus name.
  ASSERT_TRUE(response_strings_.empty());
  ASSERT_EQ("", error_names_[0]);
}

TEST_F(EndToEndAsyncTest, EmptyResponseCallback) {
  const char* kHello = "hello";

  // Create the method call.
  MethodCall method_call("org.chromium.TestInterface", "Echo");
  MessageWriter writer(&method_call);
  writer.AppendString(kHello);

  // Call the method with an empty callback.
  const int timeout_ms = ObjectProxy::TIMEOUT_USE_DEFAULT;
  object_proxy_->CallMethod(&method_call,
                            timeout_ms,
                            ObjectProxy::EmptyResponseCallback());
  // Post a delayed task to quit the message loop.
  message_loop_.PostDelayedTask(FROM_HERE,
                                base::MessageLoop::QuitClosure(),
                                TestTimeouts::tiny_timeout());
  message_loop_.Run();
  // We cannot tell if the empty callback is called, but at least we can
  // check if the test does not crash.
}

TEST_F(EndToEndAsyncTest, TestSignal) {
  const char kMessage[] = "hello, world";
  // Send the test signal from the exported object.
  test_service_->SendTestSignal(kMessage);
  // Receive the signal with the object proxy. The signal is handled in
  // EndToEndAsyncTest::OnTestSignal() in the main thread.
  WaitForTestSignal();
  ASSERT_EQ(kMessage, test_signal_string_);
}

TEST_F(EndToEndAsyncTest, TestSignalFromRoot) {
  const char kMessage[] = "hello, world";
  // Object proxies are tied to a particular object path, if a signal
  // arrives from a different object path like "/" the first object proxy
  // |object_proxy_| should not handle it, and should leave it for the root
  // object proxy |root_object_proxy_|.
  test_service_->SendTestSignalFromRoot(kMessage);
  WaitForTestSignal();
  // Verify the signal was not received by the specific proxy.
  ASSERT_TRUE(test_signal_string_.empty());
  // Verify the string WAS received by the root proxy.
  ASSERT_EQ(kMessage, root_test_signal_string_);
}

TEST_F(EndToEndAsyncTest, TestHugeSignal) {
  const std::string kHugeMessage(kHugePayloadSize, 'o');

  // Send the huge signal from the exported object.
  test_service_->SendTestSignal(kHugeMessage);
  // This caused a DCHECK failure before. Ensure that the issue is fixed.
  WaitForTestSignal();
  ASSERT_EQ(kHugeMessage, test_signal_string_);
}

TEST_F(EndToEndAsyncTest, DisconnectedSignal) {
  bus_->GetDBusTaskRunner()->PostTask(FROM_HERE,
                                      base::Bind(&Bus::ClosePrivateConnection,
                                                 base::Unretained(bus_.get())));
  // OnDisconnected callback quits message loop.
  message_loop_.Run();
  EXPECT_EQ(1, on_disconnected_call_count_);
}

class SignalMultipleHandlerTest : public EndToEndAsyncTest {
 public:
  SignalMultipleHandlerTest() {
  }

  virtual void SetUp() {
    // Set up base class.
    EndToEndAsyncTest::SetUp();

    // Connect the root object proxy's signal handler to a new handler
    // so that we can verify that a second call to ConnectSignal() delivers
    // to both our new handler and the old.
    object_proxy_->ConnectToSignal(
        "org.chromium.TestInterface",
        "Test",
        base::Bind(&SignalMultipleHandlerTest::OnAdditionalTestSignal,
                   base::Unretained(this)),
        base::Bind(&SignalMultipleHandlerTest::OnAdditionalConnected,
                   base::Unretained(this)));
    // Wait until the object proxy is connected to the signal.
    message_loop_.Run();
  }

 protected:
  // Called when the "Test" signal is received, in the main thread.
  // Copy the string payload to |additional_test_signal_string_|.
  void OnAdditionalTestSignal(Signal* signal) {
    MessageReader reader(signal);
    ASSERT_TRUE(reader.PopString(&additional_test_signal_string_));
    message_loop_.Quit();
  }

  // Called when connected to the signal.
  void OnAdditionalConnected(const std::string& interface_name,
                             const std::string& signal_name,
                             bool success) {
    ASSERT_TRUE(success);
    message_loop_.Quit();
  }

  // Text message from "Test" signal delivered to additional handler.
  std::string additional_test_signal_string_;
};

TEST_F(SignalMultipleHandlerTest, TestMultipleHandlers) {
  const char kMessage[] = "hello, world";
  // Send the test signal from the exported object.
  test_service_->SendTestSignal(kMessage);
  // Receive the signal with the object proxy.
  WaitForTestSignal();
  // Verify the string WAS received by the original handler.
  ASSERT_EQ(kMessage, test_signal_string_);
  // Verify the signal WAS ALSO received by the additional handler.
  ASSERT_EQ(kMessage, additional_test_signal_string_);
}

}  // namespace dbus
