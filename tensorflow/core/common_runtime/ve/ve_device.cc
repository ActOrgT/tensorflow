#include "tensorflow/core/common_runtime/device_factory.h"
#include "tensorflow/core/common_runtime/local_device.h"
#include "tensorflow/core/common_runtime/visitable_allocator.h"
#include "tensorflow/core/common_runtime/process_state.h"
#include "tensorflow/core/common_runtime/bfc_allocator.h"
//#include "tensorflow/core/platform/stream_executor.h"
#include "tensorflow/core/common_runtime/dma_helper.h"

#include "tensorflow/core/common_runtime/ve/ve_device.h"

#include "ve_offload.h"
#include <sys/types.h>
#include <sys/syscall.h>

#define LOCK_VEO

extern "C" {
  extern void set_fake_tid(long) __attribute__((weak));
}

namespace tensorflow {

namespace {

class VEO {
  public:
    struct Args {
      struct veo_args* args;
      Args() : args(veo_args_alloc()) { }
      ~Args() { veo_args_free(args); }
    };

    VEO() {}
    virtual ~VEO();

    uint64_t alloc_mem(size_t size) {
      VLOG(2) << "VEO::alloc_mem: tid=" << syscall(SYS_gettid);
      VLOG(2) << "VEO::alloc_mem: proc_=" << proc_ << " size=" << size;
      uint64_t addr;

      int ret;
      {
        ret = veo_alloc_mem(proc_, &addr, size);
      }

      VLOG(2) << "VEO::alloc_mem: ret=" << ret << " addr=" << std::hex << addr;
      return addr;
    }

    int free_mem(uint64_t addr) {
      return veo_free_mem(proc_, addr);
    }

    int write_mem(uint64_t ve_addr, const void* vh_buff, size_t len) {
      return veo_write_mem(proc_, ve_addr, vh_buff, len);
    }

    int read_mem(void* vh_buff, uint64_t ve_addr, size_t len) {
      return veo_read_mem(proc_, vh_buff, ve_addr, len);
    }

    Status compute(const std::string& name, const void* arg, size_t len) {
      VLOG(2) << "VEO::compute: name=" << name;
      auto it = kernel_map_.find(name);
      if (it == kernel_map_.end())
        return errors::Internal("Kernel not found: ", name);

      uint64_t sym = it->second;

      Args a;
      veo_args_set_stack(a.args, VEO_INTENT_IN, 0, (char*)arg, len);
      veo_args_set_i64(a.args, 1, len);

      int req_id = veo_call_async(ctx_, sym, a.args);
      VLOG(2) << "VEO::compute: return from veo_call_async. req_id=" << req_id;
      if (req_id == VEO_REQUEST_ID_INVALID)
        return errors::Internal("Failed to call kernel");

      VLOG(2) << "VEO::compute: call veo_wait_result for req_id=" << req_id;
      uint64_t retval;
      int ret = veo_call_wait_result(ctx_, req_id, &retval);
      VLOG(2) << "VEO::compute: return from veo_wait_result. req_id=" << req_id << " ret=" << ret << " retval=" << retval;
      if (ret != 0)
        return errors::Internal("Failed to wait kernel result");
      if (retval != 0)
        return errors::Internal("Failed in the kernel");

      return Status::OK();
    }

    Status init(int nodeid);

  private:
    pid_t proc_pid_;
    struct veo_proc_handle* proc_;
    struct veo_thr_ctxt *ctx_;

    std::map<std::string, uint64_t> kernel_map_;
};

Status veo_sym_call(struct veo_proc_handle* proc,
                    struct veo_thr_ctxt* ctx,
                    uint64_t lib_id,
                    const char* name,
                    uint64_t* retval)
{
  uint64_t sym = veo_get_sym(proc, lib_id, name);
  if (!sym)
    return errors::Internal("Failed to get symbol for ", name);

  VEO::Args args;

  if (!args.args)
    return errors::Internal("Failed to allocate arguments");

  int req_id = veo_call_async(ctx, sym, args.args);
  //VLOG(2) << "VEO::load_kernel_syms: VEO request ID = " << req_id;
  if (req_id == VEO_REQUEST_ID_INVALID) {
    return errors::Internal("Failed to call VE");
  }

  int ret = veo_call_wait_result(ctx, req_id, retval);
  if (ret != 0) {
    return errors::Internal("Failed to call wait result");
  }

  return Status::OK();
}

Status load_kernel_syms(struct veo_proc_handle* proc,
                        struct veo_thr_ctxt* ctx,
                        uint64_t lib_id,
                        std::map<std::string, uint64_t>& map)
{
  Status s;

  uint64_t num_kernels;
  s = veo_sym_call(proc, ctx, lib_id, "get_num_kernels", &num_kernels);
  if (!s.ok())
    return s;
  VLOG(2) << "VEO::load_kernel_syms: num_kernels=" << num_kernels;

  uint64_t addr;
  s = veo_sym_call(proc, ctx, lib_id, "get_kernel_table_addr", &addr);
  if (!s.ok())
    return s;

  struct kernel {
    char name[256];
    char func[256];
  } table[num_kernels];

  int ret = veo_read_mem(proc, table, addr, num_kernels * sizeof(kernel));
  if (ret != 0)
    return errors::Internal("Failed to read mem");

  for (int i = 0; i < num_kernels; ++i) {
    uint64_t sym = veo_get_sym(proc, lib_id, table[i].func);
    VLOG(2) << "VEO::load_kernel_syms:"
      << " name=" << table[i].name
      << " func=" << table[i].func
      << " sym=" << (void*)sym;
    if (!sym)
      return errors::Internal("Failed to get symbol for ", table[i].func);
    map[table[i].name] = sym;
  }

  return Status::OK();
}

Status VEO::init(int nodeid) {
  VLOG(2) << "VEO::init: pid=" << getpid() << " tid=" << syscall(SYS_gettid);
  const char* filename = NULL ;

  if (const char* tmp = getenv("VEO_KERNEL")) {
    filename = tmp;
  }
  VLOG(2) << "VEO::init: filename=" << filename;
  VLOG(2) << "VEO::init: nodeid=" << nodeid;

  proc_ = veo_proc_create(nodeid);
  VLOG(2) << "VEO::init: proc_=" << proc_;
  if (!proc_)
    return errors::Internal("Failed to create VEO proc");

  proc_pid_ = getpid();

  VLOG(2) << "VEO::init: pid=" << proc_pid_ << " tid=" << syscall(SYS_gettid);

  uint64_t lib_id = NULL ;
  if( filename != NULL ) {
    lib_id = veo_load_library(proc_, filename);
    VLOG(2) << "VEO::init: lib_id=" << lib_id;
    if (!lib_id)
      return errors::Internal("Failed to load library: ", filename);
  }

  ctx_ = veo_context_open(proc_);
  VLOG(2) << "VEO::init: ctx_=" << ctx_;
  if (!ctx_)
    return errors::Internal("Failed to open VEO context");

  return load_kernel_syms(proc_, ctx_, lib_id, kernel_map_);
}

VEO::~VEO() {
  VLOG(2) << "VEO::~VEO";
  veo_context_close(ctx_);
  veo_proc_destroy(proc_);
}

class VEMemAllocator : public SubAllocator {
  public:
    VEMemAllocator(VEO* veo) : veo_(veo) {}
    ~VEMemAllocator();
    void* Alloc(size_t alignments, size_t num_bytes);
    void Free(void* ptr, size_t num_bytes);

  private:
    VEO* veo_;
};

VEMemAllocator::~VEMemAllocator() {}

void* VEMemAllocator::Alloc(size_t alignments, size_t num_bytes) {
  VLOG(2) << "VEMemAllocator::Alloc: alignments=" << alignments << " num_bytes=" << num_bytes;
#if 0
  return veo_->alloc_mem(num_bytes);
#else
#if 0
  void* p = veo_->alloc_mem(num_bytes);
  if (reinterpret_cast<intptr_t>(p) % alignments != 0) {
    VLOG(2) << "VEMemAllocator::Alloc: alignment error. addr=" << p;
    return NULL;
  }
#endif

  size_t n = num_bytes + alignments + sizeof(intptr_t);
  uint64_t addr = veo_->alloc_mem(n);
  uint64_t addr0 = (addr + sizeof(uint64_t) + alignments) & ~(alignments - 1);
  VLOG(2) << "VEMemAllocator::Alloc addr=" << std::hex << addr
    << " addr0=" << std::hex << addr0;
  *reinterpret_cast<uint64_t*>(addr0 - sizeof(uint64_t)) = addr;
  return reinterpret_cast<void*>(addr0);
#endif
}

void VEMemAllocator::Free(void* ptr, size_t num_bytes) {
  VLOG(2) << "VEMemAllocator::Free: ptr=" << ptr;

  uint64_t addr = *reinterpret_cast<uint64_t*>(ptr - sizeof(uint64_t));

  VLOG(2) << "VEMemAllocator::Free: addr=" << std::hex << addr;

  veo_->free_mem(addr);
}


class VEBFCAllocator : public BFCAllocator {
  public:
    VEBFCAllocator(size_t total_memory, bool allow_growth, const string& name, 
                   VEO* veo);

  private:
};

VEBFCAllocator::VEBFCAllocator(size_t total_memory, bool allow_growth, const string& name, VEO* veo) :
  BFCAllocator(new VEMemAllocator(veo), total_memory, allow_growth, name) {
  }

class VEDeviceContextImpl : public VEDeviceContext {
  public:
    VEDeviceContextImpl(VEO* veo) : veo_(veo) {}

    virtual void CopyCPUTensorToDevice(const Tensor* cpu_tensor, Device* device,
                                       Tensor* device_tensor,
                                       StatusCallback done) const override;

    virtual void CopyDeviceTensorToCPU(const Tensor* device_tensor, StringPiece edge_name,
                                       Device* device, Tensor* cpu_tensor,
                                       StatusCallback done) override;

    virtual Status Compute(const std::string& name, const void* arg, size_t len);

  private:
    VEO* veo_;
};

#ifdef LOCK_VEO
class VEDeviceContextImplLock : public VEDeviceContextImpl {
  public:
    VEDeviceContextImplLock(VEO* veo) : VEDeviceContextImpl(veo) {}

    virtual void CopyCPUTensorToDevice(const Tensor* cpu_tensor, Device* device,
                                       Tensor* device_tensor,
                                       StatusCallback done) const override;

    virtual void CopyDeviceTensorToCPU(const Tensor* device_tensor, StringPiece edge_name,
                                       Device* device, Tensor* cpu_tensor,
                                       StatusCallback done) override;

    virtual Status Compute(const std::string& name, const void* arg, size_t len);

  private:
    mutable mutex lock_;
};
#endif

class VEDevice : public LocalDevice {
  public:
    VEDevice(const SessionOptions& options, const string name,
             Allocator* ve_allocator,
             Allocator* cpu_allocator) :
      LocalDevice(options,
                  Device::BuildDeviceAttributes(name, "VE",
                                                Bytes(256 << 20),
                                                DeviceLocality())),
      cpu_allocator_(cpu_allocator),
      ve_allocator_(ve_allocator) {}

    ~VEDevice() override;

    Status Init(const SessionOptions& options, VEO* veo);
    Status Sync() override { return Status::OK(); }

    Allocator* GetAllocator(AllocatorAttributes attr) override {
      if (attr.on_host())
        return cpu_allocator_;
      else
        return ve_allocator_;
    }

    Status MakeTensorFromProto(const TensorProto& tensor_proto,
                               const AllocatorAttributes alloc_attrs,
                               Tensor* tensor) override;

  protected:
    Allocator* ve_allocator_;
    Allocator* cpu_allocator_;

  private:
    GpuDeviceInfo* gpu_device_info_;
    std::vector<VEDeviceContextImpl*> device_contexts_;
};

VEDevice::~VEDevice() {
  delete gpu_device_info_;
  for (auto ctx : device_contexts_) ctx->Unref();
}

Status VEDevice::Init(const SessionOptions& options, VEO* veo) {
  VLOG(2) << "VEDevice::Init";
#ifdef LOCK_VEO
  if (getenv("TF_NOLOCK_VEO")) {
  VLOG(2) << "VEDevice::Init: VEO without lock is used";
    device_contexts_.push_back(new VEDeviceContextImpl(veo));
  } else {
  VLOG(2) << "VEDevice::Init: VEO with lock is used";
    device_contexts_.push_back(new VEDeviceContextImplLock(veo));
  }
#else
  device_contexts_.push_back(new VEDeviceContextImpl(veo));
#endif

  VLOG(2) << "VEDevice::Init DeviceContext=" << device_contexts_.back();

  gpu_device_info_ = new GpuDeviceInfo;
  gpu_device_info_->default_context = device_contexts_[0];
  set_tensorflow_gpu_device_info(gpu_device_info_);

  return Status::OK();
}

Status VEDevice::MakeTensorFromProto(const TensorProto& tensor_proto,
                                     const AllocatorAttributes alloc_attrs,
                                     Tensor* tensor) {
  VLOG(2) << "VEDeviceContextImpl::MakeTensorFromProto";
  AllocatorAttributes attr;
  attr.set_on_host(true);
  Allocator* host_alloc = GetAllocator(attr);

  Tensor parsed(tensor_proto.dtype());
  if (!parsed.FromProto(host_alloc, tensor_proto)) {
    return errors::InvalidArgument("Cannot parse tensor from proto: ",
                                   tensor_proto.DebugString());
  }
  Status status;
  if (alloc_attrs.on_host()) {
    *tensor = parsed;
  } else {
    Tensor copy(GetAllocator(alloc_attrs), parsed.dtype(), parsed.shape());

    // If the tensor is not initialized, we likely ran out of memory.
    if (!copy.IsInitialized()) {
      return errors::ResourceExhausted(
          "OOM when allocating tensor of shape ", parsed.shape().DebugString(),
          " and type ", DataTypeString(parsed.dtype()));
    }

    device_contexts_[0]->CopyCPUTensorToDevice(
        &parsed, this, &copy, [&status](const Status& s) { status = s; });
    *tensor = copy;
  }
  return status;
}

class VEOFactory {
  public:
    Status GetOrCreate(VEO** pveo, int nodeid) {
      mutex_lock guard(lock_);

      if (!veo_) {
        veo_ = new VEO;
        Status s = veo_->init(nodeid);
        if (!s.ok())
          return s;
      }

      *pveo = veo_;
      return Status::OK();
    }

    static VEOFactory* Global() {
      static VEOFactory* instance = new VEOFactory;
      return instance;
    }

  private:
    mutex lock_;
    VEO* veo_ = NULL;

    VEOFactory() = default;
    TF_DISALLOW_COPY_AND_ASSIGN(VEOFactory);
};

class VEDeviceFactory : public DeviceFactory {
  Status CreateDevices(const SessionOptions& options, const string& name_prefix,
                       std::vector<Device*>* devices) override {
    const string device_name = strings::StrCat(name_prefix, "/device:VE:0");
    VLOG(2) << "VEDeviceFactory::CreateDevices: " << device_name;

    int nodeid = 0;
    if (const char* tmp = getenv("VE_NODE_NUMBER")) {
      nodeid = atoi(tmp);
    }

    if (nodeid < 0) // user disables VE
      return Status::OK();

    VEO* veo = NULL;
    Status s = VEOFactory::Global()->GetOrCreate(&veo, nodeid);
    if (!s.ok())
      return s;

    size_t total_memory = 20UL*1024*1024*1024;
    Allocator* ve_allocator = new VEBFCAllocator(total_memory, true, "VE_0_bfc", veo);

    int numa_node = 0;

    VEDevice* device = new VEDevice(options, device_name, ve_allocator,
                                    ProcessState::singleton()->GetCPUAllocator(numa_node));
    TF_RETURN_IF_ERROR(device->Init(options, veo));
    devices->push_back(device);
    return Status::OK();
  }
};

} // namespace

REGISTER_LOCAL_DEVICE_FACTORY("VE", VEDeviceFactory, 220);

void VEDeviceContextImpl::CopyCPUTensorToDevice(const Tensor* cpu_tensor, Device* device,
                                                Tensor* device_tensor,
                                                StatusCallback done) const {
  VLOG(2) << "VEDeviceContextImpl::CopyCPUTensorToDevice";

  const void* in = DMAHelper::base(cpu_tensor);
  void* out = DMAHelper::base(device_tensor);

  VLOG(2) << "VEDeviceContextImpl::CopyCPUTensorToDevice: in=" << in << " out=" << out;
  VLOG(2) << "VEDeviceContextImpl::CopyCPUTensorToDevice: proc_pid_=" << getpid() << " tid=" << syscall(SYS_gettid);

  int rc = veo_->write_mem((uint64_t)out, in, cpu_tensor->TotalBytes());
  VLOG(2) << "VEDeviceContextImpl::CopyCPUTensorToDevice: rc=" << rc;

  done(Status::OK());
}

void VEDeviceContextImpl::CopyDeviceTensorToCPU(const Tensor* device_tensor, StringPiece edge_name,
                                                Device* device, Tensor* cpu_tensor,
                                                StatusCallback done) {
  VLOG(2) << "VEDeviceContextImpl::CopyDeviceTensorToCPU";

  const void* in = DMAHelper::base(device_tensor);
  void* out = DMAHelper::base(cpu_tensor);

  int rc = veo_->read_mem(out, (uint64_t)in, device_tensor->TotalBytes());

  VLOG(2) << "VEDeviceContextImpl::CopyDeviceTensorToCPU: rc=" << rc;

  done(Status::OK());
}

Status VEDeviceContextImpl::Compute(const std::string& name, const void* arg, size_t len)
{
  VLOG(2) << "VEDeviceContextImpl::Compute: name=" << name;
  return veo_->compute(name, arg, len);
}

#ifdef LOCK_VEO
void VEDeviceContextImplLock::CopyCPUTensorToDevice(const Tensor* cpu_tensor, Device* device,
                                                Tensor* device_tensor,
                                                StatusCallback done) const {
  mutex_lock guard(lock_);
  VEDeviceContextImpl::CopyCPUTensorToDevice(cpu_tensor, device, device_tensor, done);
}

void VEDeviceContextImplLock::CopyDeviceTensorToCPU(const Tensor* device_tensor, StringPiece edge_name,
                                                Device* device, Tensor* cpu_tensor,
                                                StatusCallback done) {
  mutex_lock guard(lock_);
  VEDeviceContextImpl::CopyDeviceTensorToCPU(device_tensor, edge_name, device, cpu_tensor, done);
}

Status VEDeviceContextImplLock::Compute(const std::string& name, const void* arg, size_t len)
{
  mutex_lock guard(lock_);
  return VEDeviceContextImpl::Compute(name, arg, len);
}
#endif

} // namespace tensorflow

