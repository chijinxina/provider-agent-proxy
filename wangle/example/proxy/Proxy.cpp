/*
 * Copyright 2017-present Facebook, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <gflags/gflags.h>

#include <folly/init/Init.h>
#include <wangle/bootstrap/ClientBootstrap.h>
#include <wangle/bootstrap/ServerBootstrap.h>
#include <wangle/channel/AsyncSocketHandler.h>

using namespace folly;
using namespace wangle;
using namespace std;

DEFINE_int32(port, 30000, "proxy server port");
DEFINE_string(remote_host, "127.0.0.1", "remote host");
DEFINE_int32(remote_port, 20880, "remote port");

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

  void readEOF(Context*) override
  {
      LOG(INFO) << "Connection closed by remote host";
      frontendPipeline_->close();
  }

  void readException(Context*, exception_wrapper e) override
  {
      LOG(ERROR) << "Remote error: " << exceptionStr(e);
      frontendPipeline_->close();
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

  DefaultPipeline::Ptr newPipeline(std::shared_ptr<AsyncTransportWrapper> sock) override
  {
    auto pipeline = DefaultPipeline::create();
    pipeline->addBack(AsyncSocketHandler(sock));
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
//HandlerAdapter<folly::IOBufQueue&, std::unique_ptr<folly::IOBuf>>
class ProxyFrontendHandler : public BytesToBytesHandler {
 public:
  explicit ProxyFrontendHandler(SocketAddress remoteAddress) : remoteAddress_(remoteAddress) {}

  //写数据到 backendPipeline_ （proxy client connect pipeline）中去
  void read(Context*, IOBufQueue& q) override
  {
    backendPipeline_->write(q.move());
  }

  //连接关闭
  void readEOF(Context* ctx) override
  {
    LOG(INFO) << "Connection closed by local host";
    backendPipeline_->close()
            .then([this, ctx]()
                  {
                    this->close(ctx);
                  });
  }

  //读取数据发生异常
  void readException(Context* ctx, exception_wrapper e) override
  {
    LOG(ERROR) << "Local error: " << exceptionStr(e);
    backendPipeline_->close().then([this, ctx](){
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

    client_.connect(remoteAddress_)
      .then(
              [this, frontendPipeline](DefaultPipeline* pipeline)
              {
                  backendPipeline_ = pipeline;
                  // Resume read
                  frontendPipeline->transportActive();  //TCP client连接成功，恢复pipeline的数据传输和处理
              })
      .onError(
              [this, ctx](const std::exception& e)
              {
                  LOG(ERROR) << "Connect error: " << exceptionStr(e);
                  this->close(ctx);
              });
  }

 private:
  SocketAddress remoteAddress_;  //远程服务器地址
  ClientBootstrap<DefaultPipeline> client_;  //连接远程服务器的客户端
  DefaultPipeline* backendPipeline_;   //客户端与远程服务器之间建立的TCP连接的pipeline
};

/*
 * TCP Server
 * TCP Server的每条连接的pipelineFactory
 */
class ProxyFrontendPipelineFactory : public PipelineFactory<DefaultPipeline> {
 public:
  explicit ProxyFrontendPipelineFactory(SocketAddress remoteAddress) : remoteAddress_(remoteAddress) {}

  DefaultPipeline::Ptr newPipeline(std::shared_ptr<AsyncTransportWrapper> sock) override
  {
    auto pipeline = DefaultPipeline::create();
    pipeline->addBack(AsyncSocketHandler(sock));
    pipeline->addBack(std::make_shared<ProxyFrontendHandler>(remoteAddress_));
    pipeline->finalize();

    return pipeline;
  }
 private:
  SocketAddress remoteAddress_;
};



/*
 * 主函数
 * Demo
 */
int main(int argc, char** argv) {
  folly::Init init(&argc, &argv);
  cout<<"ProviderAgent start!!!"<<endl;
  ServerBootstrap<DefaultPipeline> server;  //创建ServerBootstrap
  server.childPipeline(std::make_shared<ProxyFrontendPipelineFactory>(SocketAddress(FLAGS_remote_host, FLAGS_remote_port)));
  server.bind(FLAGS_port);
  server.waitForStop();
  return 0;
}
