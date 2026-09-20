//===-- DataBreakpointInfoRequestHandler.cpp ------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "DAP.h"
#include "DAPError.h"
#include "EventHelper.h"
#include "Protocol/ProtocolTypes.h"
#include "RequestHandler.h"
#include "lldb/API/SBAddress.h"
#include "lldb/API/SBMemoryRegionInfo.h"
#include "lldb/lldb-defines.h"
#include "llvm/ADT/StringExtras.h"
#include <optional>

using namespace lldb_dap;
using namespace lldb;
using namespace lldb_dap::protocol;

static bool IsRW(DAP &dap, lldb::addr_t load_addr) {
  if (!lldb::SBAddress(load_addr, dap.target).IsValid())
    return false;
  lldb::SBMemoryRegionInfo region;
  lldb::SBError err =
      dap.target.GetProcess().GetMemoryRegionInfo(load_addr, region);
  // Only lldb-server supports "qMemoryRegionInfo". So, don't fail this
  // request if SBProcess::GetMemoryRegionInfo returns error.
  if (err.Success()) {
    if (!(region.IsReadable() || region.IsWritable())) {
      return false;
    }
  }
  return true;
}

std::vector<DataBreakpointAccessType> static GetBreakpointAccessTypes(
    SBMemoryRegionInfo region) {
  std::vector<DataBreakpointAccessType> types;
  if (region.IsReadable())
    types.emplace_back(eDataBreakpointAccessTypeRead);
  if (region.IsWritable())
    types.emplace_back(eDataBreakpointAccessTypeWrite);
  if (region.IsReadable() && region.IsWritable())
    types.emplace_back(eDataBreakpointAccessTypeReadWrite);

  return types;
}

llvm::Expected<DataBreakpointInfoResponseBody> static GetAddressBreakpointInfo(
    DAP &dap, const DataBreakpointInfoArguments &args) {
  const llvm::StringRef raw_address = args.name;
  uint32_t byte_size = args.bytes.value_or(dap.target.GetAddressByteSize());

  lldb::addr_t load_addr = LLDB_INVALID_ADDRESS;
  if (raw_address.getAsInteger<lldb::addr_t>(0, load_addr)) {
    return llvm::make_error<DAPError>(
        llvm::formatv("{} is not a invalid address", args.name),
        llvm::inconvertibleErrorCode(), false);
  }

  if (!IsRW(dap, load_addr))
    return llvm::make_error<DAPError>(
        llvm::formatv(
            "memory region for address {:x} has no read or write permissions",
            load_addr),
        llvm::inconvertibleErrorCode(), false);

  lldb::SBMemoryRegionInfo region;
  lldb::SBError err =
      dap.target.GetProcess().GetMemoryRegionInfo(load_addr, region);
  std::vector<protocol::DataBreakpointAccessType> access_types =
      GetBreakpointAccessTypes(region);

  protocol::DataBreakpointInfoResponseBody response;
  if (err.Fail()) {
    response.dataId = std::nullopt;
    response.description = err.GetCString();
    return response;
  }

  if (access_types.empty()) {
    response.dataId = std::nullopt;
    response.description = llvm::formatv(
        "memory region for address {:x} has no read or write permissions",
        load_addr);
    return response;
  }

  response.dataId = llvm::formatv("{:x-}/{}/0", load_addr, byte_size);
  response.description =
      llvm::formatv("{} bytes at {:x}", byte_size, load_addr);
  response.accessTypes = std::move(access_types);

  return response;
}

namespace lldb_dap {

/// Obtains information on a possible data breakpoint that could be set on an
/// expression or variable. Clients should only call this request if the
/// corresponding capability supportsDataBreakpoints is true.
llvm::Expected<protocol::DataBreakpointInfoResponseBody>
DataBreakpointInfoRequestHandler::Run(
    const protocol::DataBreakpointInfoArguments &args) const {
  if (args.asAddress)
    return GetAddressBreakpointInfo(dap, args);

  protocol::DataBreakpointInfoResponseBody response;
  var_ref_t child_ref =
      args.variablesReference.value_or(var_ref_t(var_ref_t::k_no_child));

  SBValue variable = dap.reference_storage.FindVariable(child_ref, args.name);
  lldb::addr_t addr = LLDB_INVALID_ADDRESS;
  uint32_t size{};

  if (variable.IsValid()) {
    lldb::addr_t load_addr = variable.GetLoadAddress();
    size_t byte_size = variable.GetByteSize();
    if (load_addr == LLDB_INVALID_ADDRESS) {
      response.description =
          llvm::formatv("{0} does not exist in memory, its location is {1}",
                        variable.GetName(), variable.GetLocation());
      return response;
    }
    if (byte_size == 0) {
      response.description =
          llvm::formatv("{} byte size is 0", variable.GetName());
      return response;
    }
    addr = load_addr;
    size = byte_size;

  } else if (lldb::SBFrame frame = dap.GetLLDBFrame(args.frameId);
             child_ref.Reference() == 0 && frame.IsValid()) {
    lldb::SBValue value = frame.EvaluateExpression(args.name.c_str());
    if (value.GetError().Fail()) {
      lldb::SBError error = value.GetError();
      llvm::StringRef error_cstr = error.GetCString();
      if (error_cstr.empty())
        error_cstr = "evaluation failed";
      response.description = error_cstr.str();
      return response;
    }

    uint64_t load_addr = value.GetValueAsUnsigned();
    lldb::SBData data = value.GetPointeeData();
    if (!data.IsValid()) {
      response.description = llvm::formatv(
          "unable to get byte size for expression: {}", args.name);
      return response;
    }

    if (!IsRW(dap, load_addr)) {
      response.description = llvm::formatv(
          "memory region for address {:x} has no read or write permissions",
          load_addr);
      return response;
    }

    child_ref = dap.reference_storage.Insert(value, false, false);
    size = data.GetByteSize();
    addr = load_addr;
  } else {
    response.description = "variable not found: " + args.name;
    return response;
  }

  response.dataId =
      llvm::formatv("{:x-}/{}/{}", addr, size, child_ref.AsUInt32());
  response.accessTypes = {protocol::eDataBreakpointAccessTypeRead,
                          protocol::eDataBreakpointAccessTypeWrite,
                          protocol::eDataBreakpointAccessTypeReadWrite};
  response.description =
      llvm::formatv("{} bytes at {:x} {}", size, addr, args.name);

  return response;
}

} // namespace lldb_dap
