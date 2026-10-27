<p align="center">
  <img src="assets/logo.png" alt="LockFace" width="128">
</p>

<h1 align="center">LockFace</h1>

<p align="center">用普通摄像头登录 Windows</p>

<p align="center">
  <img src="https://img.shields.io/badge/Windows-10%20%2F%2011%20x64-0078D4" alt="Windows 10 / 11 x64">
  <a href="https://github.com/Link2323/LockFace/releases"><img src="https://img.shields.io/github/v/release/Link2323/LockFace" alt="Latest release"></a>
  <a href="LICENSE"><img src="https://img.shields.io/badge/license-MIT-blue.svg" alt="MIT License"></a>
</p>

[English](README.md) · 简体中文

LockFace 给 Windows 登录界面加了一个“人脸登录”选项。按下任意按键，人脸识别通过后进入桌面。USB 摄像头或笔记本内置摄像头都可以使用，不需要专用红外摄像头。

- **本地运行**：检测、人脸匹配和活体判断都在本机完成，不上传人脸信息，不保存照片或视频。
- **多角度录入**：支持正面、左转和右转三个角度。
- **按需开相机**：发起识别后才打开摄像头，锁屏等待时不采集画面。
- **支持本地和微软账户**：需要账户有 Windows 登录密码；原有密码登录方式保留。
- **双语界面**：简体中文和英文，可跟随系统或手动选择。

## 使用

需要 Windows 10 21H2+ / Windows 11（x64）、摄像头和管理员权限。识别速度与稳定性受 CPU、摄像头和光线影响。

1. 从 [Releases](https://github.com/Link2323/LockFace/releases) 下载 `LockFaceSetup.exe` 并安装，建议保留默认安装目录。
2. 以管理员身份打开 `LockFaceConsole.exe`，阅读隐私说明，按提示采集人脸并输入 Windows 账户密码保存。
3. 按 `Win + L` 锁屏，按下任意普通按键开始识别。

安装或识别遇到问题，参阅[故障排查](docs/operations/operations.md)，其中包含相机占用、日志位置和 Windows 智能应用控制拦截等情况。

卸载时重新运行安装程序，切换到“卸载”页，或运行卸载程序，会清理程序和保存的人脸数据。

## 密码与隐私

LockFace 通过 Windows Credential Provider 完成登录，因此需要在本机保存账户凭据。保存的内容包括账户标识、人脸特征向量和加密后的 Windows 密码。密码由 SYSTEM 服务使用 Windows DPAPI 用户作用域保护，数据目录仅允许 SYSTEM 和管理员访问。

普通摄像头配合双模型活体检测，可以降低照片和屏幕回放攻击的成功率，但不保证防住所有攻击。密码加密无法抵抗 SYSTEM 权限被攻破或整盘离线攻击。请保留账户密码，不要在高安全要求的环境中依赖本软件。

[完整隐私说明](docs/legal/privacy-notice.txt) · [英文版隐私说明](docs/legal/privacy-notice.en.txt) · [凭据存储与保护](docs/contracts/users-dat.md)

## 开发与反馈

核心使用 C++20，安装器使用 Go / Wails 和 Vue，录入界面使用 WebView2。模型推理基于 ONNX Runtime。

| 文档 | 内容 |
|---|---|
| [开发导航](DEVELOPMENT.md) | 代码入口、模块边界、验证命令 |
| [构建指南](docs/BUILD.md) | 编译、模型准备、安装包构建 |
| [认证服务](docs/modules/face-service.md) | 认证流程与摄像头生命周期 |
| [Windows 客户端](docs/modules/windows-clients.md) | 登录界面与人脸录入 |
| [安装器](docs/modules/installer.md) | 安装、升级与卸载 |
| [模型许可](docs/model-licenses.md) | 模型来源与分发条件 |

欢迎提交 Issue 或 PR。报告识别问题时，请附上摄像头型号、CPU、Windows 版本、光线条件和复现步骤，便于排查。日志中可能含账户标识，提交前请先脱敏，不要上传密码、人脸数据或凭据数据库。

## 致谢与许可

LockFace 源自 [EthanZer0/FaceLogin](https://github.com/EthanZer0/FaceLogin)，自 2026 年 10 月起独立维护。

自有代码使用 [MIT License](LICENSE)，© 2026 Link2323 & EthanZer0。第三方代码与预训练模型遵守各自许可，详见[第三方声明](THIRD_PARTY_NOTICES.txt)和[模型许可](docs/model-licenses.md)。
