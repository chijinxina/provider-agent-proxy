//
// Created by chijinxin on 18-6-5.
//
#include <iostream>

#include <folly/init/Init.h>
//#include <folly/AtomicHashMap.h>

#include <wangle/service/Service.h>
#include <wangle/service/ExpiringFilter.h>
#include <wangle/service/ClientDispatcher.h>
#include <wangle/bootstrap/ClientBootstrap.h>
#include <wangle/channel/AsyncSocketHandler.h>
#include <wangle/codec/LengthFieldBasedFrameDecoder.h>
#include <wangle/codec/LengthFieldPrepender.h>
#include <wangle/channel/EventBaseHandler.h>

using namespace std;
using namespace folly;
using namespace wangle;

class Bonk{
public:
    int64_t req_id;             //dubbo request id
    string interfaceName;       //Service interface name
    string method;              //service method
    string parameterTypesString;//service method parameter type
    string parameter;           //service method parameter
};

class Xtruct{
public:
    int64_t resp_id; //dubbo response id
    string result;   //response result
};

using SerializePipeline = wangle::Pipeline<IOBufQueue&, Bonk>;


DEFINE_int32(port, 20880, "test server port");
DEFINE_string(host, "127.0.0.1", "test server address");


/*
 * Dubbo协议序列化处理句柄
 */
class DubboRpcClientSerializeHandler : public wangle::Handler<std::unique_ptr<folly::IOBuf>,
                                                      Xtruct,
                                                      Bonk,
                                                      std::unique_ptr<folly::IOBuf> >
{
public:
    void read(Context* ctx, std::unique_ptr<folly::IOBuf> ioBuf) override
    {
       // cout<<"process DubboRpcClientSerializeHandler -> read!!!"<<endl;
        if(ioBuf)
        {
            ioBuf->coalesce();  //移动IO缓冲区链中的所有数据到一个单一的连续缓冲区内
            Xtruct received;
            for(int i=0;i<8;i++)
            {
                *( (char*)&received.resp_id + 7 - i ) = *( (const char*)ioBuf->data() + 4 + i );
            }

            received.result.assign((const char*)ioBuf->data()+16,ioBuf->length()-16);
            //cout<<"[DubboRpcClientSerializeHandler] Read: request_id="<<received.resp_id<<" result="<<received.result<<endl;
            ctx->fireRead(received);
        }
    }

    folly::Future<folly::Unit> write(Context* ctx, Bonk b) override
    {
        string request;
        //1. Dubbo version
        request.append("\"2.0.1\"");  request.append("\n");
        //2. InterfaceName
        request.append("\"");
        request.append(b.interfaceName);
        request.append("\"");         request.append("\n");
        //3. Service version
        request.append("null");       request.append("\n");
        //4. Method name
        request.append("\"");
        request.append(b.method);
        request.append("\"");         request.append("\n");
        //5. Method parameter types
        request.append("\"");
        request.append(b.parameterTypesString);
        request.append("\"");         request.append("\n");
        //6. parameter args
        request.append("\"");
        request.append(b.parameter);
        request.append("\"");         request.append("\n");
        //7. attachment
        request.append("{\"path\":\"");
        request.append(b.interfaceName);
        request.append("\"}");        request.append("\n");
       // cout<<"[DubboRpcClientSerializeHandler] Write: request_id="<<b.req_id<<" result="<<request<<endl;
        int len = request.length();

       // char headData[16];
        headData[0]=0xda;
        headData[1]=0xbb; //Magic
        headData[2]=0x06;  headData[2] |= (1<<7);  headData[2] |= (1<<6);
        headData[3]=0x00; //Status(not use in request)
        //request id (8 byte)
        for(int i=0;i<8;i++)
        {
            headData[4+i] = *((char*)&b.req_id + 7 - i);
        }
        //data length (4 byte)
        for(int i=15;i>=12;i--)
        {
            headData[i] = *(((char*)&len)+(15-i));
        }

        auto buf = folly::IOBuf::copyBuffer(headData,16);

        buf->appendChain(folly::IOBuf::copyBuffer(request.data(),request.length()));

        return ctx->fireWrite(std::move(buf));
    }

private:
    char headData[16];
};

/*
 * Dubbo rpc pipeline factory
 */
class DubboRpcPipelineFactory : public PipelineFactory<SerializePipeline> {
public:
    SerializePipeline::Ptr newPipeline(std::shared_ptr<AsyncTransportWrapper> socket) override
    {
        auto pipeline = SerializePipeline::create();
        pipeline->addBack(AsyncSocketHandler(socket));
        pipeline->addBack(EventBaseHandler()); //ensure we can write from any thread
        pipeline->addBack(LengthFieldBasedFrameDecoder(4,4096,12,0,0));
        pipeline->addBack(DubboRpcClientSerializeHandler());
        pipeline->finalize();
        return pipeline;
    }
};

/*
 * Client multiplex dispatcher.  Uses Bonk.req_id as request ID
 */
class BonkMultiplexClientDispatcher : public ClientDispatcherBase<SerializePipeline, Bonk, Xtruct>
{
public:
    void read(Context*, Xtruct in) override
    {
        //cerr<<"read resp_id="<<in.resp_id<<endl;
        auto search = requests_.find(in.resp_id);
        if(search==requests_.end()) return;
        //CHECK(search != requests_.end());
        auto p = std::move(search->second);
        requests_.erase(in.resp_id);
        p.setValue(in);
    }

    Future<Xtruct> operator()(Bonk arg) override {

        auto& p = requests_[arg.req_id];
    //    auto& p = requests_.find(arg.req_id)->second;
        auto f = p.getFuture();
        p.setInterruptHandler([arg, this](const folly::exception_wrapper&) {
            this->requests_.erase(arg.req_id);
        });
        this->pipeline_->write(arg);

        return f;
    }

    // Print some nice messages for close
    Future<Unit> close() override {
        cout<<"Channel closed\n";
        return ClientDispatcherBase::close();
    }

    Future<Unit> close(Context* ctx) override {
        cout<<"Channel closed\n";
        return ClientDispatcherBase::close(ctx);
    }

private:
    //folly::AtomicHashMap<int64_t, Promise<Xtruct>> requests_;  //request id 实现rpc通道复用
    std::unordered_map<int64_t, Promise<Xtruct>> requests_;  //request id 实现rpc通道复用
};


int main(int argc, char** argv)
{
    folly::Init init(&argc, &argv);
    ClientBootstrap<SerializePipeline> client;

    client.group(std::make_shared<folly::IOThreadPoolExecutor>(4));

    cout<<"Dubbo RPC test!!!"<<endl;

    client.pipelineFactory(std::make_shared<DubboRpcPipelineFactory>());

    auto pipeline = client.connect(SocketAddress(FLAGS_host, FLAGS_port)).get();
    cout<<"connect to dubbo server success!!!!"<<endl;
    auto dispatcher = std::make_shared<BonkMultiplexClientDispatcher>();
    dispatcher->setPipeline(pipeline);
    // Set an idle timeout of 5s using a filter.
    ExpiringFilter<Bonk, Xtruct> service(dispatcher);
    std::atomic_long id={0L};
    int a=1;
  //  try {
        while (true)
        {
           // cin>>a;
            id++;
          //  cout<<id<<endl;
            Bonk request;
            request.req_id = id;
            request.interfaceName="com.alibaba.dubbo.performance.demo.provider.IHelloService";
            request.method="hash";
            request.parameterTypesString="Ljava/lang/String;";
            request.parameter="123456";
            service(std::move(request)).then([request](Xtruct response) {
                std::cout <<"Response: id=" <<request.req_id<<","<<response.resp_id <<" result="<<response.result<< std::endl;
            });
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
//    }
//    catch (const std::exception& e)
//    {
//        std::cout << exceptionStr(e) << std::endl;
//    }
    return 0;
}