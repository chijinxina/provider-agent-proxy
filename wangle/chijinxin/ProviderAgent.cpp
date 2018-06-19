#include <folly/init/Init.h>

#include <wangle/bootstrap/ClientBootstrap.h>
#include <wangle/bootstrap/ServerBootstrap.h>
#include <wangle/channel/AsyncSocketHandler.h>
#include <wangle/channel/EventBaseHandler.h>


using namespace folly;
using namespace wangle;
using namespace std;

DEFINE_int32(ProviderAgentPort, 30000, "provider agent server port");
DEFINE_string(DubboHost, "127.0.0.1", "dubbo remote host");
DEFINE_int32(DubboPort, 20880, "dubbo remote port");
DEFINE_int32(threadpool, 2, "io threadpool size");
DEFINE_string(logs, "/root/logs", "logs dir");


/*
 * TCP client
 * 与远程服务器连接的TC客户端pipeline
 */
class ProxyBackendHandler : public InboundBytesToBytesHandler
{
public:
    explicit ProxyBackendHandler(DefaultPipeline* frontendPipeline) : frontendPipeline_(frontendPipeline) {}

    void read(Context*, IOBufQueue& q) override
    {
        frontendPipeline_->write(q.move());
    }

private:
    DefaultPipeline* frontendPipeline_;
};

/*
 * TCP client
 * 与远程服务器连接的TC客户端pipelineFactory
 */
class ProxyBackendPipelineFactory : public PipelineFactory<DefaultPipeline> {
public:
    explicit ProxyBackendPipelineFactory(DefaultPipeline* frontendPipeline) : frontendPipeline_(frontendPipeline) {}

    DefaultPipeline::Ptr newPipeline(std::shared_ptr<AsyncTransportWrapper> socket) override
    {
        auto pipeline = DefaultPipeline::create();
        pipeline->addBack(AsyncSocketHandler(socket));
        pipeline->addBack(EventBaseHandler()); //ensure we can write from any thread
        pipeline->addBack(ProxyBackendHandler(frontendPipeline_));
        pipeline->finalize();

        return pipeline;
    }
private:
    DefaultPipeline* frontendPipeline_;
};

/*
 * TCP Server
 * TCP Server的每条连接的pipeline
 */
class ProxyFrontendHandler : public BytesToBytesHandler {
public:
    //构造函数
    explicit ProxyFrontendHandler(SocketAddress remoteAddress, std::shared_ptr<IOThreadPoolExecutor> ioExecutor)
            : remoteAddress_(remoteAddress), ioExecutor_(ioExecutor)
    {}
    //写数据到 backendPipeline_ (provider-agent-client -> dubbo-server)中去
    void read(Context*, IOBufQueue& q) override
    {
        backendPipeline_->write(q.move());
    }

    //连接关闭
    void readEOF(Context* ctx) override
    {
        cout<<"consumer-agent want to close connnection!!!"<<endl;
        backendPipeline_->close()
                .then([this, ctx]()
                      {
                          this->close(ctx);
                      });
    }

    //控制pipeline的数据传输（暂停：transportInactive； 运行：transportActive）
    void transportActive(Context* ctx) override   //恢复连接
    {
        if (backendPipeline_)
        {
            // Already connected
            return;
        }

        // Pause reading from the socket until remote connection succeeds
        auto frontendPipeline = dynamic_cast<DefaultPipeline*>(ctx->getPipeline());  //从ctx上下文获取当前的pipeline

        frontendPipeline->transportInactive();  //暂停pipeline数据传输和处理

        client_.pipelineFactory(std::make_shared<ProxyBackendPipelineFactory>(frontendPipeline));  //创建client pipelineFactory
        client_.group(ioExecutor_);
        try
        {
            client_.connect(remoteAddress_).via(ioExecutor_.get())
                    .then(
                            [this, frontendPipeline](DefaultPipeline* pipeline)
                            {
                                cout<<"connect to dubbo success!!!"<<endl;
                                backendPipeline_ = pipeline;
                                // Resume read
                                frontendPipeline->transportActive();  //TCP client连接成功，恢复pipeline的数据传输和处理
                            })
                    .onError(
                            [this, ctx](const std::exception& e)
                            {
                                cout << "Connect to dubbo error: " << exceptionStr(e);
                            });
        }
        catch (const std::exception& e)
        {
            cout << "Connect to dubbo error: " << exceptionStr(e);
        }

    }

private:
    SocketAddress remoteAddress_;  //远程服务器地址
    ClientBootstrap<DefaultPipeline> client_;  //连接远程服务器的客户端
    DefaultPipeline* backendPipeline_;   //客户端与远程服务器之间建立的TCP连接的pipeline
    std::shared_ptr<IOThreadPoolExecutor> ioExecutor_;
};

/*
 * TCP Server
 * TCP Server的每条连接的pipelineFactory
 */
class ProxyFrontendPipelineFactory : public PipelineFactory<DefaultPipeline> {
public:
    //ProxyFrontendPipelineFactory构造函数
    explicit ProxyFrontendPipelineFactory(SocketAddress remoteAddress, std::shared_ptr<IOThreadPoolExecutor> ioExecutor)
            : remoteAddress_(remoteAddress),ioExecutor_(ioExecutor)
    {}

    DefaultPipeline::Ptr newPipeline(std::shared_ptr<AsyncTransportWrapper> sock) override
    {
        auto pipeline = DefaultPipeline::create();
        pipeline->addBack(AsyncSocketHandler(sock));
        pipeline->addBack(EventBaseHandler()); //ensure we can write from any thread
        pipeline->addBack(std::make_shared<ProxyFrontendHandler>(remoteAddress_, ioExecutor_));
        pipeline->finalize();

        return pipeline;
    }
private:
    SocketAddress remoteAddress_;
    std::shared_ptr<IOThreadPoolExecutor> ioExecutor_;
};



int main(int argc, char** argv)
{
    folly::Init init(&argc, &argv);
    cout<<"ProviderAgent start!!!"<<endl;

    std::shared_ptr<IOThreadPoolExecutor> IOWorkThreadPool = std::make_shared<IOThreadPoolExecutor>(2);

    ServerBootstrap<DefaultPipeline> providerAsyncTcpServer;  //创建ServerBootstrap
    providerAsyncTcpServer.childPipeline(
            std::make_shared<ProxyFrontendPipelineFactory>(SocketAddress(FLAGS_DubboHost, FLAGS_DubboPort), IOWorkThreadPool));
    providerAsyncTcpServer.group(nullptr,IOWorkThreadPool);
    providerAsyncTcpServer.bind(FLAGS_ProviderAgentPort);
    providerAsyncTcpServer.waitForStop();
    return 0;
}
