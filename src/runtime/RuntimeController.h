#pragma once

#include <cstdint>
#include <functional>

namespace runtime {

// 控制服务启动参数。
struct ServerOptions {
    std::uint32_t port = 19090;
};

// ---------------------------------------------------------------------------
// RuntimeController
//
// 职责：
// 1) 对外提供 TCP 控制协议（INIT/START/STOP/STATUS/QUIT）
// 2) 管理运行时状态机（未初始化/初始化中/就绪/运行中/初始化失败）
// 3) 通过回调与业务层解耦（不直接依赖 TencentBot 内部细节）
//
// 回调约定：
// - init_fn: 执行初始化，失败抛异常
// - run_fn : 执行主业务（阻塞直到结束/停止），失败抛异常
// - stop_fn: 请求业务层停止（应尽量快速返回）
// ---------------------------------------------------------------------------
class RuntimeController {
public:
    using InitFn = std::function<void()>;
    using RunFn = std::function<void()>;
    using StopFn = std::function<void()>;

    RuntimeController(ServerOptions options, InitFn init_fn, RunFn run_fn, StopFn stop_fn);

    // 阻塞运行控制服务：
    // 返回 0 表示正常退出，非 0 表示初始化/网络等错误。
    int RunBlocking();

private:
    ServerOptions options_;
    InitFn init_fn_;
    RunFn run_fn_;
    StopFn stop_fn_;
};

} // namespace runtime
