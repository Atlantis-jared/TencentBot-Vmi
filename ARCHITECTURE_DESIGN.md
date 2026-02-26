# Host-Guest 零延迟物理内存注入架构重构 (Direct Memory Overwrite Protocol)

这个方案的核心思想是：**“控制平面走 vsock，数据平面走纯物理内存”**。

仅仅在脚本刚启动时，通过你现有的 vsock 通道做一次极简的“握手（交货地址确认）”；一旦握手成功，之后的千万次坐标读写将完全脱离网络通信，Host(PVE) 像“上帝之手”一样直接物理篡改 Guest(Windows) 进程的内存。

## 1. 结构体设计 (Guest 端 C++ 数据接收区)

在 `TencentBot-vmi` (Guest端) 的 C++ 代码里，我们需要预先分配一个专属的、防止被编译器优化掉的内存结构体，用来充当“收货信箱”。

```cpp
#pragma pack(push, 1) // 保证内存紧凑对齐，无多余补位
struct SharedDataStatus {
    volatile uint32_t sync_flag;   // 状态标志位：0=未就绪，1=Host已接管并开始写入，2=请求停止
    volatile uint64_t timestamp;   // Host最后一次写入的时间戳(微秒)，用于Guest判断数据是否卡死
    
    // 以下为实际的游戏数据(如坐标)
    volatile int32_t current_x;
    volatile int32_t current_y;
    volatile int32_t map_id;
};
#pragma pack(pop)

// 全局静态分配，确保生命周期和进程一样长
SharedDataStatus g_shared_data = {0}; 
```
*注：加入 `volatile` 关键字极其重要，它能防止 C++ 编译器把这个变量放入 CPU 寄存器缓存，强制要求程序每次循环都去物理内存里真实读取（因为这段内存会被外部的 Host 偷偷改掉，编译器是不知情的）。*

---

## 2. 初始化握手阶段 (控制平面 - vsock)

初始化握手分三步走，这个过程通过你现存的 `vsock` 管道完成。

### Step 1: Guest (Bot) 发起 `INIT_BIND` 请求
当 Bot 启动时，它首先向 Host 的 vsock 服务端发送一个 JSON 格式的绑定请求。

**请求包格式 (Guest -> Host)**:
```json
{
  "cmd": "INIT_BIND",
  "data": {
    "bot_pid": 12345,                  // 当前 TencentBot 进程的 PID
    "bot_receive_addr": "0x00AABBCC",  // g_shared_data 变量在 Bot 进程里的首地址(虚拟地址)
    
    "target_game_name": "xyq.exe",     // 目标游戏进程名
    "target_base_addr": "0x12345678",  // 目标坐标的基址或基址模块名(或者由Host自己去扫特征码)
    "target_offsets": [0x10, 0x20]     // 如果是指针，提供偏移量数组
  }
}
```

### Step 2: Host (PVE) 物理页表解析与准备
Host 收到这个 JSON 后，立刻开始执行底层操作（代码在 `TencentBot-vmi-webui` 的底层库中实现）：
1. **解析游戏地址**：根据 `target_game_name`，读取游戏 CR3 (页目录基址)，结合偏移，算出坐标的 **宿主机物理地址 (HPA_Game)**。
2. **解析 Bot 地址**：根据 `bot_pid` 和 `bot_receive_addr` (收货地址)，读取 Bot 的 CR3，算出收货变量在宿主机上的 **物理偏移量 (HPA_Bot)**。
3. **写入同步位 (心跳激活)**：Host 定位到 `HPA_Bot` 的前 4 个字节，直接写入整数 `1` (把 `sync_flag` 改为 1)。

### Step 3: Host 回复 `READY`，握手结束
Host 向 Guest 发送最后一条 vsock 消息。

**回复包格式 (Host -> Guest)**:
```json
{
  "status": "success",
  "msg": "HOST_READY_DATA_LINK_ESTABLISHED"
}
```

至此，**vsock 通道进入静默期，不再走高频收发，初始化完成！**

---

## 3. 极速死循环阶段 (数据平面 - 物理内存读写)

握手一旦完成，双方进入“背靠背”工作模式。

### Host 侧守护进程 (死循环，高频刷新 - 技术栈：memflow-py)：
Host 在 Linux 环境里跑一个死循环（纯物理内存操作，不占用网络栈）。
**后端技术栈要求**：使用 **memflow-py** (Memflow 的 Python 绑定，专门针对 KVM/QEMU)。它提供了顶级的跨虚拟机内存解析能力。

```python
import memflow

# 初始化 memflow 连接到指定的 KVM 虚拟机
inventory = memflow.Inventory()
os = inventory.create_os("qemu", "vm101")  # 连接到目标虚拟机

while True:
    # 1. 瞬间从游戏进程物理内存 (HPA_Game) 抠出 12 字节的坐标数据
    #    (这里演示虚拟地址读取，memflow会自动做页表翻译转换为物理地址读取)
    game_bytes = game_process.read_bytes(vaddr_game, 12)
    
    # 2. 构造带时间戳的同步包裹 (4字节头部特征码/FLAG + 8字节时间戳 + 12字节坐标)
    payload = struct.pack("<I q 12s", 1, current_micro_time(), game_bytes)
    
    # 3. 瞬间暴力覆盖到 Bot 的接收区
    bot_process.write_bytes(vaddr_bot, payload)
    
    time.sleep(0.01) # 休眠10毫秒
```

### Guest 侧 Bot 脚本 (正常的无感业务逻辑)：
在 `TencentBot-vmi` 跑商代码里，不需要写任何网络接收代码，直接读本地结构体变量即可。

```cpp
void RunStep() {
    // 检查 Host 底层管道是否断开
    if (g_shared_data.sync_flag != 1) {
        printf("等待 Host 端进行底层物理内存接管...\n");
        return;
    }

    // 疯狂读它！没有任何网络时延，它是你自己的本地变量！
    printf("当前坐标: X=%d, Y=%d\n", g_shared_data.current_x, g_shared_data.current_y);
}
```
