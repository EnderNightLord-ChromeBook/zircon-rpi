// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// [START includes]
#include <fuchsia/examples/llcpp/fidl.h>
#include <lib/async-loop/cpp/loop.h>
#include <lib/async-loop/default.h>
#include <lib/fidl/llcpp/client.h>
#include <lib/service/llcpp/service.h>
#include <zircon/assert.h>

#include <iostream>
// [END includes]

// [START main]
int main(int argc, const char** argv) {
  async::Loop loop(&kAsyncLoopConfigAttachToCurrentThread);
  async_dispatcher_t* dispatcher = loop.dispatcher();

  // |service::OpenServiceRoot| returns a channel connected to the /svc directory.
  // The remote end of the channel implements the |fuchsia.io/Directory| protocol
  // and contains the capabilities provided to this component.
  auto svc = service::OpenServiceRoot();
  ZX_ASSERT(svc.is_ok());

  // Connect to the |fuchsia.examples/Echo| protocol, here we demonstrate
  // using |service::ConnectAt| relative to some service directory.
  // One may also directly call |Connect| to use the default service directory.
  auto client_end = service::ConnectAt<::llcpp::fuchsia::examples::Echo>(*svc);
  ZX_ASSERT(client_end.status_value() == ZX_OK);

  // Define the event handler for the client. The OnString event handler prints the event.
  class EventHandler : public llcpp::fuchsia::examples::Echo::AsyncEventHandler {
   public:
    explicit EventHandler(async::Loop& loop) : loop_(loop) {}

    void OnString(llcpp::fuchsia::examples::Echo::OnStringResponse* event) override {
      std::string response(event->response.data(), event->response.size());
      std::cout << "Got event: " << response << std::endl;
      loop_.Quit();
    }

   private:
    async::Loop& loop_;
  };

  // Create a client to the Echo protocol.
  fidl::Client client(std::move(*client_end), dispatcher, std::make_shared<EventHandler>(loop));

  // Make an EchoString call, passing it a lambda to handle the response asynchronously.
  auto result_async = client->EchoString(
      "hello", [&](llcpp::fuchsia::examples::Echo::EchoStringResponse* response) {
        std::string reply(response->response.data(), response->response.size());
        std::cout << "Got response: " << reply << std::endl;
        loop.Quit();
      });
  ZX_ASSERT(result_async.ok());
  loop.Run();
  loop.ResetQuit();

  // Make a synchronous EchoString call, which blocks until it receives the response,
  // then returns a ResultOf object for the response.
  auto result_sync = client->EchoString_Sync("hello");
  ZX_ASSERT(result_async.ok());
  std::string reply_string(result_sync->response.data(), result_sync->response.size());
  std::cout << "Got synchronous response: " << reply_string << std::endl;

  // Make a SendString request. The resulting OnString event will be handled by
  // the event handler defined above.
  auto result_oneway = client->SendString("hi");
  // Check for any synchronous errors.
  ZX_ASSERT(result_oneway.ok());
  loop.Run();

  return 0;
}
// [END main]
