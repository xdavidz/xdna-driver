// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2023-2024, Advanced Micro Devices, Inc. All rights reserved.

#include "hwctx.h"
#include "hwq.h"

#include "core/common/config_reader.h"
#include "core/common/memalign.h"

namespace shim_xdna {

void
hw_ctx_ipu::
create_ctx_unsecure(const device& device, const xrt::xclbin& xclbin)
{
  amdxdna_drm_create_hwctx_unsecure arg = {};
  arg.xclbin_p = reinterpret_cast<uintptr_t>(xclbin.get_axlf());

  const size_t ip_buf_sz = 4 * 1024; // should be big enough
  auto ip_buf = xrt_core::aligned_alloc(8, ip_buf_sz);
  memset(ip_buf.get(), 0, ip_buf_sz);
  arg.ip_buf_size = ip_buf_sz;
  arg.ip_buf_p = reinterpret_cast<uintptr_t>(ip_buf.get());

  arg.qos_size = sizeof(amdxdna_qos_info);
  arg.qos_p = reinterpret_cast<uintptr_t>(get_qos_info());

  device.get_pdev().ioctl(DRM_IOCTL_AMDXDNA_CREATE_HWCTX_UNSECURE, &arg);
  set_slotidx(arg.handle);
  init_cu_info(ip_buf.get());
}

void
hw_ctx_ipu::
create_ctx(const device& device, const xrt::xclbin& xclbin)
{
  amdxdna_drm_create_hwctx arg = {};
  uuid_copy(arg.xclbin_uuid, xclbin.get_uuid().get());

  const size_t ip_buf_sz = 4 * 1024; // should be big enough
  auto ip_buf = xrt_core::aligned_alloc(8, ip_buf_sz);
  memset(ip_buf.get(), 0, ip_buf_sz);
  arg.ip_buf_size = ip_buf_sz;
  arg.ip_buf_p = reinterpret_cast<uintptr_t>(ip_buf.get());

  arg.qos_size = sizeof(amdxdna_qos_info);
  arg.qos_p = reinterpret_cast<uintptr_t>(get_qos_info());

  device.get_pdev().ioctl(DRM_IOCTL_AMDXDNA_CREATE_HWCTX, &arg);
  set_slotidx(arg.handle);
  init_cu_info(ip_buf.get());
}

hw_ctx_ipu::
hw_ctx_ipu(const device& device, const xrt::xclbin& xclbin, const xrt::hw_context::qos_type& qos)
  : hw_ctx(device, qos, std::make_unique<hw_q_ipu>(device))
{
  if (xrt_core::config::detail::get_env_value("XRT_HACK_UNSECURE_LOADING_XCLBIN"))
    create_ctx_unsecure(device, xclbin);
  else
    create_ctx(device, xclbin);

  auto queue = static_cast<hw_q_ipu*>(get_hw_queue());
  queue->bind_hwctx(this);
  shim_debug("Created IPU HW context (%d)", get_slotidx());
}

hw_ctx_ipu::
~hw_ctx_ipu()
{
  auto hdl = get_slotidx();

  if (hdl == INVALID_CTX_HANDLE)
    return;

  shim_debug("Destroying IPU HW context (%d)...", hdl);
  auto queue = static_cast<hw_q_ipu*>(get_hw_queue());
  queue->unbind_hwctx();
  struct amdxdna_drm_destroy_hwctx arg = {};
  arg.handle = hdl;
  m_device.get_pdev().ioctl(DRM_IOCTL_AMDXDNA_DESTROY_HWCTX, &arg);
}

void
hw_ctx_ipu::
init_cu_info(const void *cu_idx_buf)
{
  auto names_base = static_cast<const char*>(cu_idx_buf);
  auto name_idx_pairs = reinterpret_cast<const amdxdna_ip_name_index*>(cu_idx_buf);
  std::vector<unsigned char> nullpdi;

  for (auto nm_idx = &name_idx_pairs[0]; nm_idx->name_off != 0; nm_idx++) {
    xrt_core::cuidx_type cuidx = { .index = nm_idx->index, };
    m_cu_info.insert(std::make_pair(std::string(names_base + nm_idx->name_off),
      std::make_pair(cuidx, nullpdi /* PDI is not available */)));
  }
}

} // shim_xdna
