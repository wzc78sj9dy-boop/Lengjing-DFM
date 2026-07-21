#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace lengjing::game::native {

using RemoteElfReadCallback = bool (*)(void* context,
                                       std::uintptr_t address,
                                       void* destination,
                                       std::size_t size);

bool ReadRemoteElfBuildId(std::uintptr_t imageBase,
                          RemoteElfReadCallback reader,
                          void* readerContext,
                          std::string& buildId);

}  // namespace lengjing::game::native
