#include <Messages.pb.h>
#include <metal_frontend/messages/message_header.hpp>
#include <metal_pipeline/data_source.hpp>
#include <metal_pipeline/data_sink.hpp>
#include <metal_pipeline/pipeline_runner.hpp>
#include <algorithm>
#include "pipeline_loop.hpp"
#include "registered_agent.hpp"

namespace metal {

void PipelineLoop::run() {

  // Build a PipelineDefinition
  std::vector<std::shared_ptr<AbstractOperator>> pipeline_operators;
  pipeline_operators.reserve(_pipeline.size());
  for (const auto &operator_agent : _pipeline) {
    pipeline_operators.emplace_back(operator_agent.first);
  }
  auto pipeline_definition = std::make_shared<PipelineDefinition>(std::move(pipeline_operators));

  ProfilingPipelineRunner runner(pipeline_definition, _card);

  auto dataSource = std::dynamic_pointer_cast<DataSource>(_pipeline.front().first);
  auto dataSink = std::dynamic_pointer_cast<DataSink>(_pipeline.back().first);

  auto hostMemoryDataSource = std::dynamic_pointer_cast<HostMemoryDataSource>(dataSource);
  auto hostMemoryDataSink = std::dynamic_pointer_cast<HostMemoryDataSink>(dataSink);

  // Some data sources (e.g. FileDataSource) are capable of providing the overall processing size in the beginning
  // This might only be a temporary optimization since we will allow operators to provide arbitrary output data sizes
  // in the future.
  size_t total_size = 0;
  if ((total_size = dataSource->reportTotalSize()) > 0) {
    dataSink->prepareForTotalProcessingSize(total_size);
  }

  auto firstAgentIt = std::find_if(_pipeline.cbegin(), _pipeline.cend(),
                                   [](const std::pair<std::shared_ptr<AbstractOperator>, std::shared_ptr<RegisteredAgent>> &elem) {
                                       return elem.second != nullptr;
                                   });
  assert(firstAgentIt != _pipeline.cend());

  auto lastAgentReverseIt = std::find_if(_pipeline.crbegin(), _pipeline.crend(),
                                         [](const std::pair<std::shared_ptr<AbstractOperator>, std::shared_ptr<RegisteredAgent>> &elem) {
                                             return elem.second != nullptr;
                                         });
  assert(lastAgentReverseIt != _pipeline.crend());
  auto lastAgentIt = (lastAgentReverseIt+1).base();

    // Receive ready signal from first client
  auto inputBufferMessage = firstAgentIt->second->socket.receive_message<message_type::AgentPushBuffer>();

  // Wait until all other clients signal ready as well
  for (auto operatorAgentPair = std::next(firstAgentIt, 1); operatorAgentPair != std::next(lastAgentIt, 1); ++operatorAgentPair) {
    operatorAgentPair->second->socket.receive_message<message_type::AgentPushBuffer>();
  }

  // Determine if any operator is selected for profiling
  auto profilingOperator = _pipeline.cend();
  for (auto operatorAgentPairIt = _pipeline.cbegin(); operatorAgentPairIt != _pipeline.cend(); ++operatorAgentPairIt) {
      if (operatorAgentPairIt->first->profiling_enabled()) {
          if (profilingOperator == _pipeline.cend()) {
              profilingOperator = operatorAgentPairIt;
          } else {
              throw std::runtime_error("Can only select one operator at a time for profiling");
          }
      }
  }

  if (profilingOperator != _pipeline.cend()) {
    runner.selectOperatorForProfiling(profilingOperator->first);
  }

  for (;;) {
    size_t size = 0;
    bool eof = false;

    size_t output_size = 0;

    // If the client provides us with input data, forward it to the pipeline
    if (firstAgentIt->second->input_buffer) {
      size = inputBufferMessage.size();
      eof = inputBufferMessage.eof();
      hostMemoryDataSource->setSource(firstAgentIt->second->input_buffer.value().current());
      firstAgentIt->second->input_buffer.value().swap();
    } else {
      // Data comes from an internal source (file or generator), must have a total size then
      size = total_size < BUFFER_SIZE ? total_size : BUFFER_SIZE;
      total_size -= size;
      eof = total_size == 0;
    }

    if (lastAgentIt->second->output_buffer) {
      hostMemoryDataSink->setDest(lastAgentIt->second->output_buffer.value().current());
      lastAgentIt->second->output_buffer.value().swap();
    }

    if (size) {
      dataSource->setSize(size);
      dataSink->setSize(size);

      output_size = runner.run(eof);
    }

    // Send processing responses
    for (auto currentAgentIt = firstAgentIt; currentAgentIt != std::next(lastAgentIt, 1); ++currentAgentIt) {
      ServerProcessedBuffer msg;

      if (!eof && currentAgentIt != firstAgentIt && currentAgentIt != lastAgentIt) {
        continue;
      }

      if (!eof && !currentAgentIt->second->input_buffer && !currentAgentIt->second->output_buffer) {
        continue;
      }

      msg.set_eof(eof);

      if (currentAgentIt == lastAgentIt && lastAgentIt->second->output_buffer) {
        msg.set_size(output_size);
      }

      if (currentAgentIt == profilingOperator) {
          msg.set_message(runner.formatProfilingResults());
      }

      currentAgentIt->second->socket.send_message<message_type::ServerProcessedBuffer>(msg);
    }

    if (eof) return;

    if (lastAgentIt->second->output_buffer) {
      // Wait for output client push buffer message (i.e. 'ready')
      if (firstAgentIt == lastAgentIt) {
        inputBufferMessage = firstAgentIt->second->socket.receive_message<message_type::AgentPushBuffer>();
      } else {
        lastAgentIt->second->socket.receive_message<message_type::AgentPushBuffer>();
      }
    }

    if (firstAgentIt != lastAgentIt && firstAgentIt->second->input_buffer) {
      // Wait for input client push buffer message (i.e. the next inputBufferMessage)
      inputBufferMessage = firstAgentIt->second->socket.receive_message<message_type::AgentPushBuffer>();
    }
  }
}

} // namespace metal
