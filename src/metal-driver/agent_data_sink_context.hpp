#pragma once

#include <memory>

#include <metal-filesystem-pipeline/file_data_sink_context.hpp>

namespace metal {

class FileDataSinkContext;
class FilesystemContext;
class OperatorAgent;
class Pipeline;

class AgentDataSinkContext : public FileDataSinkContext {
 public:
  explicit AgentDataSinkContext(std::shared_ptr<OperatorAgent> agent,
                                std::shared_ptr<Pipeline> pipeline,
                                bool skipReceivingProcessingRequest);

  const DataSink dataSink() const final;
  void configure(SnapAction &action, uint64_t inputSize, bool initial) final;
  void finalize(SnapAction &action, uint64_t outputSize, bool endOfInput) final;
  void prepareForTotalSize(uint64_t totalSize);

 protected:
  std::shared_ptr<OperatorAgent> _agent;
  std::shared_ptr<Pipeline> _pipeline;
  bool _skipReceivingProcessingRequest;
  uint64_t _size;
};

}  // namespace metal
