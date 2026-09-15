// 平台实现工厂：上层唯一获取实现的入口，具体实现在编译期由 CMake 按平台选择
#pragma once
#include <memory>
#include "IProcessOps.hpp"
#include "IScreenCapture.hpp"
#include "IShellSpawn.hpp"
#include "IInputInjector.hpp"
#include "ISystemInfo.hpp"

namespace echonode::platform {

std::unique_ptr<ISystemInfo> createSystemInfo();
std::unique_ptr<IProcessOps> createProcessOps();
std::unique_ptr<IShellSpawn> createShellSpawn();
std::unique_ptr<IScreenCapture> createScreenCapture();
std::shared_ptr<IInputInjector> createInputInjector();

}
