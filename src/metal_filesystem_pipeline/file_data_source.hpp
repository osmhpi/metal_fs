#include <metal_pipeline/data_source.hpp>
#include <metal_filesystem/metal.h>

namespace metal {

class FileDataSource : public CardMemoryDataSource {

  // Common API
 public:
 protected:

  void configure(SnapAction &action) override;
  void finalize(SnapAction &action) override;

  std::vector<mtl_file_extent> _extents;
  uint64_t _offset;


  // API to be used from PipelineStorage (extent list-based)
 public:
  explicit FileDataSource(std::vector<mtl_file_extent> &extents, uint64_t offset, uint64_t size);


  // API to be used when building file pipelines (filename-based)
 public:
  explicit FileDataSource(std::string filename, uint64_t offset, uint64_t size = 0);
  size_t reportTotalSize() override;

 protected:
  uint64_t loadExtents();

  std::string _filename;

};

} // namespace metal
