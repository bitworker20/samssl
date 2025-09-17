## samssl

一个基于 Boost.Asio C++20 协程的 I2P SAM 协议轻量库与示例（Echo Server/Client）。

### 功能概述
- **SamConnection**: 管理与 SAM 网关的 TCP 连接、HELLO 协商、命令/回复、数据流读写（带超时与取消）。
- **SamService**: 管理控制会话（SESSION CREATE），并在新 TCP 连接上执行 `STREAM ACCEPT`/`STREAM CONNECT`，返回可用于数据流的连接对象。
- **SamMessageParser**: 解析 HELLO/SESSION/STREAM/NAMING/DEST 各类 SAM 文本回复。
- **I2PIdentityUtils**: 私钥生成与 `.b32.i2p` 地址解析（依赖 i2pd 的 `libi2pd`）。
- **示例**: `i2p_sam_echo_server` 与 `i2p_sam_echo_client` 展示如何使用库进行流式回显通信。

### 目录结构
- 库与头文件：`SamConnection.*`, `SamService.*`, `SamMessageParser.*`, `I2PIdentityUtils.*`
- 示例：`echo_server.cpp`, `echo_client.cpp`
- 构建：`CMakeLists.txt`（通过 FetchContent 获取 `i2pd` 源码，并在其 `build/` 目录构建 `libi2pd.a`）

### 关键类型（摘录）
```35:84:/home/smart/works/github/samssl/SamService.h
class SamService : public std::enable_shared_from_this<SamService> {
public:
    net::awaitable<EstablishSessionResult> establishControlSession(...);
    net::awaitable<SetupStreamResult> acceptStreamViaNewConnection(const std::string& control_session_id);
    net::awaitable<SetupStreamResult> connectToPeerViaNewConnection(
        const std::string& control_session_id,
        const std::string& target_peer_i2p_address_b32,
        const std::map<std::string, std::string>& options = {...});
    void shutdown();
};
```

```16:65:/home/smart/works/github/samssl/SamConnection.h
class SamConnection : public std::enable_shared_from_this<SamConnection>
{
public:
    enum class ConnectionState { DISCONNECTED, CONNECTING, CONNECTED_NO_HELLO, HELLO_OK, DATA_STREAM_MODE, CLOSING, CLOSED, ERROR_STATE };
    net::awaitable<bool> connect(const std::string &host, uint16_t port, SteadyClock::duration timeout = std::chrono::seconds(10));
    net::awaitable<SAM::ParsedMessage> performHello(SteadyClock::duration timeout = std::chrono::seconds(5));
    net::awaitable<SAM::ParsedMessage> sendCommandAndWaitReply(const std::string &command, SteadyClock::duration reply_timeout = std::chrono::seconds(10));
    net::awaitable<std::string> readLine(SteadyClock::duration timeout);
    net::awaitable<std::size_t> streamRead(net::mutable_buffer buffer, SteadyClock::duration timeout = std::chrono::minutes(5));
    net::awaitable<void> streamWrite(net::const_buffer buffer, SteadyClock::duration timeout = std::chrono::seconds(30));
    void closeSocket();
};
```

### 依赖
- C++20 编译器（已在 `configure_target` 中强制 `CXX_STANDARD 20`）
- Boost（system, context, program_options, thread）
- spdlog（自动选择 `header_only` 或常规目标）
- OpenSSL, ZLIB, Threads
- i2pd（通过 FetchContent 获取源码，通过 ExternalProject 在 `i2pd/build` 内编译 `libi2pd.a`）

### 构建
```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

说明：
- CMake 会使用 FetchContent 拉取 `i2pd` 源码（仓库与分支见下），并在 `i2pd/build` 目录配置与构建，生成 `libi2pd.a`。
- 我们的库与示例在链接前依赖上述构建步骤（`add_dependencies` 已声明）。
- 如需自定义 `i2pd` 的构建选项，可在 `CMakeLists.txt` 的 `ExternalProject_Add(i2pd_project)` 段落中调整 `CONFIGURE_COMMAND` 的参数（例如是否构建可执行等）。

### 运行
构建完成后，生成以下可执行文件：
- `i2p_sam_echo_server`
- `i2p_sam_echo_client`

服务器（Echo Server）
- 参数：`<private_key_file_path>|TRANSIENT`
- 行为：
  - 当参数为文件路径时，从文件读取 Base64 私钥；
  - 当参数为 `TRANSIENT` 时，使用临时目的地（会话由 SAM 生成）。

```bash
./build/i2p_sam_echo_server TRANSIENT
# 或
./build/i2p_sam_echo_server /path/to/private_key.b64
```

客户端（Echo Client）
- 参数：`<private_key_file_path> <target_peer_i2p_address_b32>`

```bash
./build/i2p_sam_echo_client /path/to/private_key.b64 <server_address>.b32.i2p
```

交互指令（客户端）：
- 直接输入一行文本将发送给服务端并回显。
- 输入 `big N` 可发送大小为 `N*1024` 字节的载荷。
- 输入 `exit`/`quit` 或 Ctrl+C 结束。

默认 SAM 网关
- 示例中默认的 `SAM_HOST` 与 `SAM_PORT` 在源码内硬编码（`echo_server.cpp`、`echo_client.cpp`）。如需调整，请修改源码或扩展 CLI（推荐后续改造）。

### 安全与隐私
- 日志：当前日志可能包含网关回复原文，注意避免输出包含 `DESTINATION=`/`PRIV=` 的敏感信息到生产日志。
- 匿名性：默认 `inbound.length/outbound.length=1` 更偏向可用性，建议根据场景提升默认值或开放配置项。

### 已知注意事项
- `echo_server.cpp` 未校验 `argc` 即访问 `argv[1]`，请按上述用法提供参数。
- `echo_client.cpp` 在循环读之前应清理 `read_ec` 与 `bytes_read`，已在建议中标注（可在后续提交修复）。
- `SamConnection` 内部读超时计时器为成员共享，若未来引入并发读应分离为局部计时器以避免相互取消。

### 参考
- i2pd 仓库（openssl 分支）：[`https://github.com/bitworker20/i2pd.git`](https://github.com/bitworker20/i2pd.git)

### 许可证
- 本项目遵循与依赖一致的开源许可证（i2pd 为 BSD-3-Clause）。具体以各自仓库与文件头为准。


