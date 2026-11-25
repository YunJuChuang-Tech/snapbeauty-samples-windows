# SnapBeauty Windows Samples

[![License](https://img.shields.io/badge/License-Apache%202.0-blue.svg)](LICENSE)
[![Platform](https://img.shields.io/badge/Platform-Windows-blue.svg)](#环境要求)

本仓库提供 SnapBeauty SDK 1.3.6 的 Windows 接入示例。两个入口都使用公开的 `snapbeauty/desktop.h` C 接口。

| 示例 | 内容 |
| --- | --- |
| [相机美颜](windows/main.cpp) | Media Foundation 采集 RGB32 → SnapBeauty 处理 BGRA8 → Win32 / GDI 预览。可选「晨曦」妆容 |
| [纯 C 接入](examples/minimal.c) | 生成一张 RGBA8 渐变，初始化授权、创建引擎、处理一帧并释放资源 |

## 环境要求

- Windows 10 1809 及以上，x64。需要显卡厂商的 OpenGL 2.1+ 驱动及帧缓冲对象支持。
- Visual Studio x64 C++ Build Tools 和 Windows SDK。可选 LLVM `clang-cl` 或 MSVC `cl`。
- 相机示例需要可访问的摄像头。带妆容构建还需要 Python 3.9+、Pillow 和 NumPy。

## 快速开始

### 1. 解压 SDK

从 SnapBeauty 官方渠道获取 `SnapBeauty-Windows-1.3.6.zip`。将文件放在用户的 Downloads 目录，在仓库根目录执行以下命令。也可解压至任意目录，并用 `-SdkRoot` 或 `SNAPBEAUTY_SDK_ROOT` 指定包含 `include/`、`lib/`、`bin/`、`Assets/` 的目录。

```powershell
$zip = Join-Path $env:USERPROFILE 'Downloads/SnapBeauty-Windows-1.3.6.zip'
Get-FileHash -Algorithm SHA256 -LiteralPath $zip
Expand-Archive -LiteralPath $zip -DestinationPath .local/sdk
```

本示例对应 [SDK 1.3.6 ZIP](https://github.com/YunJuChuang-Tech/snapbeauty-windows-sdk/releases/download/1.3.6/SnapBeauty-Windows-1.3.6.zip)，SHA-256：`af9b7fe4d80e8b61158a738f4087c08ce6650b9da37a92672a3e4129bf2633fd`。

### 2. 构建

在 Visual Studio 的 **x64 Developer PowerShell** 中，从仓库根目录执行其中一条构建命令。`clang-cl` 需在 PATH 上。

```powershell
$sdk = (Resolve-Path '.local/sdk/SnapBeauty-Windows-1.3.6').Path
.\windows\build.ps1 -SdkRoot $sdk -OutDir build/windows -Compiler clang-cl
# 或使用 MSVC
.\windows\build.ps1 -SdkRoot $sdk -OutDir build/windows -Compiler cl
```

输出目录包含 `snapbeauty-camera.exe`、`snapbeauty-minimal.exe`、运行所需的 `snapbeauty.dll` 和 `Assets/`。每次构建请指定新输出目录。

需要演示「晨曦」时，先安装 Python 依赖，再加 `-WithMakeup`。启动对应输出目录里的相机程序后，在预览窗口按 `M` 切换。按 `0` 关闭口红，按 `1`–`5` 选择珊瑚色、西柚色、豆沙粉、元气橘、复古红；按上下方向键调节所选色号强度。口红可独立使用，无需妆容素材。

```powershell
python -m pip install Pillow numpy
.\windows\build.ps1 -SdkRoot $sdk -OutDir build/windows-makeup -WithMakeup
```

### 3. 申请授权并运行

Windows 桌面授权需与移动端分开签发。向 SnapBeauty 授权提供方提交平台 `windows`、宿主 `app_id` 和宿主 EXE 的 Authenticode 签名者证书 SHA-256 指纹。相机的默认 `app_id` 是 `ai.snapbeauty.samples.windows`，命令行示例的默认值是 `ai.snapbeauty.samples.windows.minimal`。用 `SNAPBEAUTY_APP_ID` 设置实际获签的身份。

使用代码签名证书签署将要运行的宿主 EXE，再用 SDK 的 `tools\sb-fingerprint.exe` 读取指纹。授权的 `signers` 可登记 1 到 4 个指纹。重新编译会生成新的 EXE，运行授权模式前请重新签名。

```powershell
& "$sdk/tools/sb-fingerprint.exe" .\build\windows\snapbeauty-camera.exe
$env:SNAPBEAUTY_ASSETS = (Resolve-Path build/windows/Assets).Path
$env:SNAPBEAUTY_APP_ID = 'ai.snapbeauty.samples.windows'
$env:SNAPBEAUTY_LICENSE_FILE = Join-Path $env:USERPROFILE '.snapbeauty/windows-camera.license'
.\build\windows\snapbeauty-camera.exe
```

请把授权文件放在仓库外，并将变量设为实际路径。运行妆容版时将命令中的 `build/windows` 换成 `build/windows-makeup`。如相机无法打开，在「设置 → 隐私和安全性 → 相机」检查是否允许桌面应用访问相机。

纯 C 示例可先不设置授权，确认 SDK 版本和帧处理链路。合成帧没有人脸，适合验证接口调用。

```powershell
Remove-Item Env:SNAPBEAUTY_LICENSE_FILE -ErrorAction SilentlyContinue
$env:SNAPBEAUTY_ASSETS = (Resolve-Path build/windows/Assets).Path
.\build\windows\snapbeauty-minimal.exe
```

`xm_desktop_init` 的状态 JSON 中 `code=0` 表示授权有效，`code=2` 表示缺少授权。没有有效授权时，SDK 返回原帧。若创建引擎报告 `gpu_unsupported`，请在启用了厂商驱动的本机图形会话运行。

## 许可

查看 [Apache 2.0 许可证](LICENSE)和[第三方声明](THIRD_PARTY_NOTICES.md)。
