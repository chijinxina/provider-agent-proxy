#include <wangle/codec/StringCodec.h>
#include <wangle/codec/ByteToMessageDecoder.h>

#include <wangle/service/ClientDispatcher.h>
#include <wangle/service/ServerDispatcher.h>
#include <wangle/service/Service.h>
#include <wangle/service/CloseOnReleaseFilter.h>
#include <wangle/service/ExpiringFilter.h>

using namespace wangle;
using namespace folly;

typedef Pipeline<IOBufQueue&, std::string> ServicePipeline;  //Service Pipeline

/*
 * 简单编码器
 * 字节->字节
 */
class SimpleDecode : public ByteToByteDecoder {
public:
    bool decode(Context* /*ctx*/,
                IOBufQueue& buf /*BufQueue*/,
                std::unique_ptr<IOBuf>& result /*Buf*/,
                size_t& /**/) override
    {
        result = buf.move();
        return result != nullptr;
    }
};

/*
 * 定义一个服务
 * request:string response:string
 */
class EchoService : public Service<std::string, std::string>{
public:
    Future<std::string> operator()(std::string req) override
    {
        return req;
    }
};

/*
 * ServerPipelineFactory
 */
template <typename Req, typename Resp>
class ServerPipelineFactory : public PipelineFactory<ServicePipeline> {
public:
    typename ServicePipeline::Ptr newPipeline(std::shared_ptr<AsyncTransportWrapper> socket) override
    {
        auto pipeline = ServicePipeline::create();
        pipeline->addBack(AsyncSocketHandler(socket));  //底部异步SOCKET Handel
        pipeline->addBack(SimpleDecode());               //简单编码
        pipeline->addBack(StringCodec());               //Byte to String
        pipeline->addBack(SerialServerDispatcher<Req,Resp>(&service_)); //服务Service(request-response)的处理句柄
        pipeline->finalize();
        return pipeline;
    }

private:
    EchoService service_;   //回声服务（发送字符串，回复原字符串）
};

/*
 * ClientPipelineFacory
 */
template <typename Req, typename Resp>
class ClientPipelineFactory : public PipelineFactory<ServicePipeline>{
public:
    typename ServicePipeline::Ptr newPipeline(std::shared_ptr<AsyncTransportWrapper> socket) override
    {
        auto pipeline = ServicePipeline::create();
        pipeline->addBack(AsyncSocketHandler(socket));
        pipeline->addBack(SimpleDecode());
        pipeline->addBack(StringCodec());
        pipeline->finalize();
        return pipeline;
    }
};


template <typename Pipeline, typename Req, typename Resp>
class ClientServiceFactory : public  ServiceFactory<Pipeline, Req, Resp>{
public:
    //client-service
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
        SerialClientDispatcher<Pipeline, Req, Resp> dispatcher_;
    };

    //ClientServiceFactory(std::shared_ptr<ClientBootstrap<Pipeline>> client)仿函数
    Future<std::shared_ptr<Service<Req, Resp>>> operator()(std::shared_ptr<ClientBootstrap<Pipeline>> client)override
    {
        return Future<std::shared_ptr<Service<Req, Resp>>>(std::make_shared<ClientService>(client->getPipeline()));
    };
};




int main()
{
    ServerBootstrap<ServicePipeline> server;
    server.childPipeline(std::make_shared<ServerPipelineFactory<std::string, std::string>>());
    server.bind(0);

    //service client
    auto client = std::make_shared<ClientBootstrap<ServicePipeline>>();

    ClientServiceFactory<ServicePipeline, std::string, std::string> serviceFactory;

    client->pipelineFactory(std::make_shared<ClientPipelineFactory<std::string, std::string>>());

    SocketAddress addr;
    server.getSockets()[0]->getAddress(&addr);
    //SocketAddress addr("127.0.0.1",10000);
    //server.getSockets()[0]->getAddress(&addr);
    client->connect(addr).waitVia(EventBaseManager::get()->getEventBase());

    auto service = serviceFactory(client).value();
    auto rep = (*service)("chijinxin");
    std::cout<<"chijinxin"<<std::endl;
    rep.then(
            [&](std::string value)
            {
                std::cout<<"value="<<value<<std::endl;
                EventBaseManager::get()->getEventBase()->terminateLoopSoon();
            });

    server.waitForStop();
    return 0;
}