// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2023-2024, Advanced Micro Devices, Inc. All rights reserved.

#include "bo.h"
#include "device.h"
#include "hwctx.h"
#include "drm_local/amdxdna_accel.h"

namespace {

// Device memory on IPU needs to be within one 64MB page. The maximum size is 48MB.
const size_t dev_mem_size = (48 << 20);

}

namespace shim_xdna {

device_ipu::
device_ipu(const pdev& pdev, handle_type shim_handle, id_type device_id)
: device(pdev, shim_handle, device_id)
{
  // Alloc and register device memory w/ driver.
  m_dev_heap_bo = std::make_unique<bo_ipu>(*this, dev_mem_size, AMDXDNA_BO_DEV_HEAP);
  shim_debug("Created IPU device (%s) ...", get_pdev().m_sysfs_name.c_str());
}

device_ipu::
~device_ipu()
{
  shim_debug("Destroying IPU device (%s) ...", get_pdev().m_sysfs_name.c_str());
}

std::unique_ptr<xrt_core::hwctx_handle>
device_ipu::
create_hw_context(const device& dev, const xrt::xclbin& xclbin, const xrt::hw_context::qos_type& qos) const
{
  return std::make_unique<hw_ctx_ipu>(dev, xclbin, qos);
}

std::unique_ptr<xrt_core::buffer_handle>
device_ipu::
alloc_bo(void* userptr, size_t size, uint64_t flags)
{
  if (userptr)
    shim_not_supported_err("User ptr BO");;
  return std::make_unique<bo_ipu>(*this, size, flags);
}

} // namespace shim_xdna
