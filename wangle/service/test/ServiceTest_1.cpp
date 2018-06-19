#include <wangle/codec/StringCodec.h>
#include <wangle/codec/ByteToMessageDecoder.h>
#include <wangle/service/ClientDispatcher.h>
#include <wangle/service/ServerDispatcher.h>
#include <wangle/service/Service.h>
#include <wangle/service/CloseOnReleaseFilter.h>
#include <wangle/service/ExpiringFilter.h>
#include <wangle/channel/EventBaseHandler.h>

using namespace wangle;
using namespace folly;




typedef Pipeline<IOBufQueue&, std::string> ServicePipeline;

/*
 * 简单编码器
 * 字节->字节
 */
class SimpleDecode : public ByteToByteDecoder {
public:
    bool decode(Context*,
                IOBufQueue& buf,
                std::unique_ptr<IOBuf>& result,
                size_t&) override {
        result = buf.move();
        return result != nullptr;
    }
};


/*
 * 定义一个服务
 * request:string response:string
 */
class EchoService : public Service<std::string, std::string> {
public:
    Future<std::string> operator()(std::string req) override
    {
        std::this_thread::sleep_for(std::chrono::microseconds(5000));
        return req;
    }
};

/*
 * 定义一个服务
 * request:string response:int
 */
class EchoIntService : public Service<std::string, int> {
public:
    Future<int> operator()(std::string req) override
    {
        return folly::to<int>(req);
    }
};

template <typename Req, typename Resp>
class ServerPipelineFactory : public PipelineFactory<ServicePipeline> {
public:

    typename ServicePipeline::Ptr newPipeline(
            std::shared_ptr<AsyncTransportWrapper> socket) override {
        auto pipeline = ServicePipeline::create();
        pipeline->addBack(AsyncSocketHandler(socket));
        pipeline->addBack(SimpleDecode());
        pipeline->addBack(StringCodec());
        pipeline->addBack(PipelinedServerDispatcher<Req, Resp>(&service_));
        pipeline->finalize();
        return pipeline;
    }

private:
    EchoService service_;
};

template <typename Req, typename Resp>
class ClientPipelineFactory : public PipelineFactory<ServicePipeline> {
public:

    typename ServicePipeline::Ptr newPipeline(std::shared_ptr<AsyncTransportWrapper> socket) override
    {
        auto pipeline = ServicePipeline::create();
        pipeline->addBack(AsyncSocketHandler(socket));
        pipeline->addBack(EventBaseHandler()); // ensure we can write from any thread
        pipeline->addBack(SimpleDecode());
        pipeline->addBack(StringCodec());
        pipeline->finalize();
        return pipeline;
    }
};

template <typename Pipeline, typename Req, typename Resp>
class ClientServiceFactory : public ServiceFactory<Pipeline, Req, Resp> {
public:
    class ClientService : public Service<Req, Resp> {
    public:
        explicit ClientService(Pipeline* pipeline)
        {
            dispatcher_.setPipeline(pipeline);
        }
        Future<Resp> operator()(Req request) override
        {
            return dispatcher_(std::move(request));
        }

    private:
        PipelinedClientDispatcher<Pipeline, Req, Resp> dispatcher_;
    };

    Future<std::shared_ptr<Service<Req, Resp>>> operator() (std::shared_ptr<ClientBootstrap<Pipeline>> client) override
    {
        return Future<std::shared_ptr<Service<Req, Resp>>>(std::make_shared<ClientService>(client->getPipeline()));
    }
};

int main()
{
    ServerBootstrap<ServicePipeline> server;
    server.childPipeline(std::make_shared<ServerPipelineFactory<std::string, std::string>>());
    server.bind(0);

    // client
    auto client = std::make_shared<ClientBootstrap<ServicePipeline>>();

    ClientServiceFactory<ServicePipeline, std::string, std::string> serviceFactory;

    client->pipelineFactory(std::make_shared<ClientPipelineFactory<std::string, std::string>>());

    SocketAddress addr;
    server.getSockets()[0]->getAddress(&addr);

    client->connect(addr).waitVia(EventBaseManager::get()->getEventBase());

    auto service1 = serviceFactory(client).value();

    int count=0;
    for(int i=0;i<1000000;i++)
    {
        std::ostringstream oss;
        oss<<"chijinxin"<<i<<"\n";
        (*service1)(oss.str()).then(
                [&](std::string value)
                {
                    std::cout<<value<<std::endl;
                });
        std::cout<<"Request="<<++count<<std::endl;
    }

 //   auto rep1 = (*service1)("test1");
//    //usleep(3000);
////    auto service2 = serviceFactory(client).value();
//    auto rep2 = (*service1)("test2");
////    auto service3 = serviceFactory(client).value();
////    auto rep3 = (*service3)("test3");
////    auto service4 = serviceFactory(client).value();
////    auto rep4 = (*service4)("test4");
//
//    rep1.then([&](std::string value) {
//        //EXPECT_EQ("test", value);
//        std::cout<<"value1="<<value<<std::endl;
//        //EventBaseManager::get()->getEventBase()->terminateLoopSoon();
//    });
//    rep2.then([&](std::string value) {
//        //EXPECT_EQ("test", value);
//        std::cout<<"value2="<<value<<std::endl;
//       // EventBaseManager::get()->getEventBase()->terminateLoopSoon();
//
//    });
//    rep3.then([&](std::string value) {
//        //EXPECT_EQ("test", value);
//        std::cout<<"value3="<<value<<std::endl;
//        EventBaseManager::get()->getEventBase()->terminateLoopSoon();
//
//    });
//    rep4.then([&](std::string value) {
//        //EXPECT_EQ("test", value);
//        std::cout<<"value4="<<value<<std::endl;
//        EventBaseManager::get()->getEventBase()->terminateLoopSoon();
//
//    });
  //  EventBaseManager::get()->getEventBase()->loopForever();
   EventBaseManager::get()->getEventBase()->loopForever();

    //server.stop();
    server.waitForStop();

    return 0;
}


