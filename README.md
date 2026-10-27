<p align="center">
  <img src="assets/logo.png" alt="LockFace" width="128">
</p>

<h1 align="center">LockFace</h1>

<p align="center">Sign in to Windows with an ordinary camera</p>

<p align="center">
  <img src="https://img.shields.io/badge/Windows-10%20%2F%2011%20x64-0078D4" alt="Windows 10 / 11 x64">
  <a href="https://github.com/Link2323/LockFace/releases"><img src="https://img.shields.io/github/v/release/Link2323/LockFace" alt="Latest release"></a>
  <a href="LICENSE"><img src="https://img.shields.io/badge/license-MIT-blue.svg" alt="MIT License"></a>
</p>

English · [简体中文](README.zh-CN.md)

LockFace adds a "face sign-in" option to the Windows sign-in screen. Press any key, and once your face is recognized you are on the desktop. A USB webcam or a laptop's built-in camera works — no dedicated infrared camera required.

- **Runs locally.** Face detection, matching and liveness checks all happen on your PC. Face data never leaves the machine, and no photos or videos are saved.
- **Multi-angle enrollment.** Front, left and right views.
- **Camera on demand.** The camera is activated only when recognition starts; the lock screen captures nothing while it waits.
- **Local and Microsoft accounts.** Accounts need a Windows sign-in password; the normal password sign-in path stays untouched.
- **Bilingual interface.** English and Simplified Chinese, chosen automatically from the system language or set manually.

## Getting started

You need Windows 10 21H2+ or Windows 11 (x64), a camera, and administrator rights. Recognition speed and reliability depend on your CPU, camera and lighting.

1. Download `LockFaceSetup.exe` from [Releases](https://github.com/Link2323/LockFace/releases) and install it. Keeping the default installation directory is recommended.
2. Run `LockFaceConsole.exe` as administrator, read the privacy notice, then follow the prompts to capture your face and save your Windows account password.
3. Press `Win + L` to lock the screen, then press any key to start recognition.

If installation or recognition misbehaves, see [troubleshooting](docs/operations/operations.md) (in Chinese) — it covers camera conflicts, log locations, Smart App Control blocks and more.

To uninstall, run the installer again and switch to its "Uninstall" page, or run the uninstaller; both remove the program and the saved face data.

## Passwords and privacy

LockFace signs you in through a Windows Credential Provider, which is why account credentials must be stored on the machine: account identifiers, face templates (feature vectors) and the encrypted Windows password. The password is protected by the SYSTEM service with Windows DPAPI in user scope, and the data directory grants access only to SYSTEM and Administrators.

Liveness detection on an ordinary camera uses two models, which lowers — but does not eliminate — the success rate of photo and screen-replay attacks. Password encryption does not hold up against a compromised SYSTEM account or offline full-disk attacks. Keep your account password, and do not rely on this software in high-security settings.

[Privacy notice (English)](docs/legal/privacy-notice.en.txt) · [Privacy notice (Chinese, canonical)](docs/legal/privacy-notice.txt) · [Credential storage and protection](docs/contracts/users-dat.md) (in Chinese)

## Development and feedback

The core is C++20; the installer is Go/Wails with a Vue frontend; the enrollment UI runs on WebView2. Inference is powered by ONNX Runtime. The deeper documentation is currently written in Chinese.

| Document | Contents |
|---|---|
| [Development navigation](DEVELOPMENT.md) (in Chinese) | code entry points, module boundaries, verification commands |
| [Build guide](docs/BUILD.md) (in Chinese) | compiling, model preparation, building the installer |
| [Authentication service](docs/modules/face-service.md) (in Chinese) | authentication flow and camera lifecycle |
| [Windows clients](docs/modules/windows-clients.md) (in Chinese) | lock screen and enrollment |
| [Installer](docs/modules/installer.md) (in Chinese) | installing, upgrading and uninstalling |
| [Model licenses](docs/model-licenses.md) (in Chinese) | model provenance and redistribution terms |

Issues and pull requests are welcome. When reporting recognition problems, please include your camera model, CPU, Windows version, lighting conditions and reproduction steps. Logs may contain account identifiers — redact them before submitting, and never upload passwords, face data or the credential database.

## Credits and license

LockFace started from [EthanZer0/FaceLogin](https://github.com/EthanZer0/FaceLogin) and has been maintained independently since October 2026.

The project's own code is released under the [MIT License](LICENSE), © 2026 Link2323 & EthanZer0. Third-party code and pretrained models keep their own licenses — see [third-party notices](THIRD_PARTY_NOTICES.txt) and [model licenses](docs/model-licenses.md).
