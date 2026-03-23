# LLM API Proxy

这是一个用 C++ 编写的 OpenAI 风格 LLM API 转发服务，具备以下能力：

- 启动时读取 JSON 配置文件
- 校验多个入站 `model` 白名单
- 校验多个入站 API Key
- 将请求转发到上游 OpenAI 风格接口
- 将请求与响应的关键字段、头、耗时、状态码、Body 预览同时打印到控制台并写入日志文件
- 提供 `/healthz` 健康检查接口
- 提供 `/v1/models` 模型列表接口

## 目录结构

- `CMakeLists.txt`：CMake 构建脚本
- `src/main.cpp`：服务主逻辑与 HTTP 转发
- `src/config.cpp`：JSON 配置加载
- `src/logger.cpp`：控制台和滚动文件日志初始化
- `config.example.json`：示例配置

## 配置文件

示例见 `config.example.json`。

字段说明：

- `bind_address`：监听地址，默认 `0.0.0.0`
- `listen_port`：监听端口
- `inbound_models`：允许接入的模型名列表，支持多个
- `model_mappings`：可选的模型映射表，键是入站模型名，值是实际发送到上游的模型名
- `inbound_api_keys`：允许接入的 API Key 列表，支持多个
- `outbound_api_url`：上游 OpenAI 风格 API 地址，例如 `https://api.openai.com` 或带版本前缀的 `https://dashscope.aliyuncs.com/compatible-mode/v1`
- `outbound_api_key`：上游 API Key
- `log_dir`：日志目录
- `request_body_log_limit`：请求 Body 日志截断长度
- `response_body_log_limit`：响应 Body 日志截断长度
- `upstream_timeout_seconds`：上游超时时间
- `verify_upstream_tls`：是否校验上游 TLS 证书

说明：你的原始需求里将“出站 llm api url”写了两次，这里按常见代理场景实现为一个 `outbound_api_url`。它作为上游基础地址，程序会自动兼容以下两种客户端路径：

- `http://host:port/v1/chat/completions`
- `http://host:port/chat/completions`

同时也兼容上游基础地址带或不带 `/v1` 的两种配置，避免出现重复拼接 `/v1/v1/...` 或缺失 `/v1/...` 的问题。

如果前端暴露给业务侧的模型名和上游平台实际模型名不一致，可以使用 `model_mappings` 做改写。例如前端继续传 `hajimi-Ultra`，代理转发时改写成 `qwen-plus`。

示例：

```json
{
	"inbound_models": ["hajimi-Ultra", "hajimi-mini"],
	"model_mappings": {
		"hajimi-Ultra": "qwen-plus",
		"hajimi-mini": "qwen-turbo"
	}
}
```

## 编译要求

- CMake 3.20+
- 支持 C++17 的编译器
- 系统已安装 libcurl 和 OpenSSL
- 首次构建时，CMake 会自动拉取以下依赖：
	- `cpp-httplib`
	- `nlohmann/json`
	- `spdlog`

macOS 上如缺少依赖，可先安装：

```bash
brew install cmake curl openssl
```

## 编译方法

在项目根目录执行：

```bash
cmake -S . -B build
cmake --build build -j
```

生成的可执行文件默认位于：

```bash
build/llm_api_proxy
```

## 启动方法

1. 复制示例配置：

```bash
cp config.example.json config.json
```

2. 修改 `config.json`

3. 启动服务：

```bash
./build/llm_api_proxy config.json
```

## 使用示例

### 健康检查

```bash
curl http://127.0.0.1:8080/healthz
```

### 获取模型列表

```bash
curl http://127.0.0.1:8080/v1/models \
	-H "Authorization: Bearer sk-inbound-example-1"
```

### 转发 Chat Completions

```bash
curl http://127.0.0.1:8080/v1/chat/completions \
	-H "Content-Type: application/json" \
	-H "Authorization: Bearer sk-inbound-example-1" \
	-d '{
		"model": "gpt-4o-mini",
		"messages": [
			{"role": "user", "content": "你好"}
		]
	}'
```

## 日志说明

程序会同时输出到控制台和日志文件，文件名按启动时间生成，例如 `logs/llmapi_log_20240601_120000.txt`，日志内容包括：

- 启动配置摘要
- 入站请求来源 IP、方法、路径、查询串、头信息、Body 预览
- 鉴权失败与参数校验失败详情
- 上游请求耗时、状态码、响应头、响应 Body 预览
- 异常栈对应的错误信息

出于安全考虑，日志中对 API Key 做了脱敏处理，但仍建议不要在生产环境里把日志权限开得过宽。

## 当前实现边界

- 已支持 OpenAI 风格的常见 HTTP 路由转发，例如 `/v1/chat/completions`、`/v1/embeddings`、`/v1/responses`
- 当前对流式响应采用普通 HTTP 响应透传，不做逐块实时推送优化
- 若请求 JSON 中存在 `model` 字段，会校验其是否在白名单内
- 若配置了 `model_mappings`，代理会在转发前自动将请求体中的 `model` 改写为对应的上游模型名

如果你后续要继续增强，优先建议补这几项：

1. 上游流式 SSE 实时转发
2. 更细粒度的路由级访问控制
3. 日志按天切分与 JSON 结构化输出
4. Prometheus 指标导出
