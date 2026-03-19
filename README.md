# 构建（Windows 32 位）

此脚本在 Windows 上自动化以下步骤以生成 32 位可执行：

- 克隆并引导 `vcpkg`（如果缺失）
- 使用 `vcpkg` 安装 `openssl:x86-windows`
- 使用 `vcpkg` 工具链配置并生成 Win32（`x86`）CMake 构建
- 将运行时 DLL（如 `libcrypto-3.dll`/`libssl-3.dll`）复制到输出目录
- 将 `config.example.json` 复制为 `config.json` 到输出目录

使用方法

在项目根目录打开 PowerShell，运行：

```powershell
.
\buildwin.ps1
```

可选参数（示例）：

```powershell
.
\buildwin.ps1 -VcpkgDir ".\vcpkg" -BuildDir ".\build_x86" -Triplet "x86-windows" -Config "Release"
```

前提依赖

- 已安装 `git`, `cmake`, 和 Visual Studio（包含用于 Win32 的 C/C++ 工具链）。

注意

- 脚本会在本仓库根目录创建 `vcpkg` 目录并安装包（需要网络与磁盘空间）。
- 若要在 CI 中使用，请确保 CI 机器安装了相应的 Visual Studio 组件或改用交叉编译工具链。
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