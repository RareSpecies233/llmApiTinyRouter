# LLM API Proxy

一个使用 JSON 配置的 C++ OpenAI 兼容中转服务。

## 功能

- 校验入站 `Bearer token` 与请求体中的 `model` 是否存在精确匹配配置
- 不匹配时拒绝转发
- 匹配时把请求中的 `model` 替换为出站模型
- 按 OpenAI 风格接口把请求转发到上游 API

## 配置格式

复制 `config.example.json` 为 `config.json` 后修改：

```json
{
  "server": {
    "host": "0.0.0.0",
    "port": 8080
  },
  "routes": [
    {
      "inbound_key": "sk-in-001",
      "inbound_model": "gpt-4o-mini",
      "outbound_key": "sk-out-001",
      "outbound_model": "deepseek-chat",
      "outbound_api_base": "https://api.openai.com"
    }
  ]
}
```

## 构建

```bash
cmake -S . -B build
cmake --build build
```

## 运行

```bash
./build/llm_api_proxy config.json
```

## 支持的请求

- `POST /v1/chat/completions`
- `POST /v1/completions`
- `POST /v1/responses`

请求头需要包含：

```text
Authorization: Bearer <inbound_key>
Content-Type: application/json
```

请求体需要包含：

```json
{
  "model": "<inbound_model>"
}
```

如果 `inbound_key` 和 `inbound_model` 不匹配任何配置项，服务会返回 `403`。