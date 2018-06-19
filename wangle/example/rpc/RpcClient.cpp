#include <gflags/gflags.h>

#include <folly/init/Init.h>
#include <wangle/service/Service.h>
#include <wangle/service/ExpiringFilter.h>
#include <wangle/service/ClientDispatcher.h>
#include <wangle/bootstrap/ClientBootstrap.h>
#include <wangle/channel/AsyncSocketHandler.h>
#include <wangle/codec/LengthFieldBasedFrameDecoder.h>
#include <wangle/codec/LengthFieldPrepender.h>
#include <wangle/channel/EventBaseHandler.h>
#include <wangle/channel/Pipeline.h>

#include <wangle/example/rpc/ClientSerializeHandler.h>

using namespace folly;
using namespace wangle;


//using SerializePipeline = wangle::Pipeline<IOBufQueue&, Bonk>;
using SerializePipeline = wangle::Pipeline<IOBufQueue&, std::string>;

DEFINE_int32(port, 20880, "test server port");
DEFINE_string(host, "127.0.0.1", "test server address");

class RpcPipelineFactory : public PipelineFactory<SerializePipeline> {
 public:
  SerializePipeline::Ptr newPipeline(std::shared_ptr<AsyncTransportWrapper> sock) override
  {
    auto pipeline = SerializePipeline::create();
    pipeline->addBack(AsyncSocketHandler(sock));
    // ensure we can write from any thread
    pipeline->addBack(EventBaseHandler());
    pipeline->addBack(LengthFieldBasedFrameDecoder());
    pipeline->addBack(LengthFieldPrepender());
    pipeline->addBack(ClientSerializeHandler());  //自己实现
    pipeline->finalize();
    return pipeline;
  }
};

// Client multiplex dispatcher.  Uses Bonk.type as request ID
//class BonkMultiplexClientDispatcher : public ClientDispatcherBase<SerializePipeline, Bonk, Xtruct>
class MultiplexRpcClientDispatcher : public ClientDispatcherBase<SerializePipeline, std::string, std::string>
{
 public:
//  void read(Context*, Xtruct in) override
    void read(Context*, std::string in) override
    {
      auto search = requests_.find(in.i32_thing);
      CHECK(search != requests_.end());
      auto p = std::move(search->second);
      requests_.erase(in.i32_thing);
      p.setValue(in);
    }

 // Future<Xtruct> operator()(Bonk arg) override {
 Future<std::string> operator()(std::string arg) override
 {
   auto& p = requests_[arg.type];
   auto f = p.getFuture();
   p.setInterruptHandler(
           [arg, this](const folly::exception_wrapper&)
           {
             this->requests_.erase(arg.type);
           });
    this->pipeline_->write(arg);
    return f;
 }

  // Print some nice messages for close

  Future<Unit> close() override
  {
    printf("Channel closed\n");
    return ClientDispatcherBase::close();
  }

  Future<Unit> close(Context* ctx) override
  {
    printf("Channel closed\n");
    return ClientDispatcherBase::close(ctx);
  }

 private:
  std::unordered_map<int32_t, Promise<Xtruct>> requests_;
};

int main(int argc, char** argv) {
  folly::Init init(&argc, &argv);

  /**
   * For specific protocols, all the following code would be wrapped
   * in a protocol-specific ServiceFactories.
   *
   * TODO: examples of ServiceFactoryFilters, for connection pooling, etc.
   */
  ClientBootstrap<SerializePipeline> client;
  client.group(std::make_shared<folly::IOThreadPoolExecutor>(1));
  client.pipelineFactory(std::make_shared<RpcPipelineFactory>());
  auto pipeline = client.connect(SocketAddress(FLAGS_host, FLAGS_port)).get();
  // A serial dispatcher would assert if we tried to send more than one
  // request at a time
  // SerialClientDispatcher<SerializePipeline, Bonk> service;
  // Or we could use a pipelined dispatcher, but responses would always come
  // back in order
  // PipelinedClientDispatcher<SerializePipeline, Bonk> service;
  auto dispatcher = std::make_shared<BonkMultiplexClientDispatcher>();
  dispatcher->setPipeline(pipeline);

  // Set an idle timeout of 5s using a filter.
  ExpiringFilter<Bonk, Xtruct> service(dispatcher, std::chrono::seconds(5));

  try {
    while (true) {
      std::cout << "Input string and int" << std::endl;

      Bonk request;
      std::cin >> request.message;
      std::cin >> request.type;
      service(request).then([request](Xtruct response) {
        CHECK(request.type == response.i32_thing);
        std::cout << response.string_thing << std::endl;
      });
    }
  } catch (const std::exception& e) {
    std::cout << exceptionStr(e) << std::endl;
  }

  return 0;
}
