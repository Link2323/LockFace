# 安装器模块

安装、卸载界面和进度支持简体中文与英文；切换语言会重新要求阅读对应隐私说明。机器级语言设置、文案入口与验证见 [`localization.md`](localization.md)。

目录：`installer/LockFaceSetup/`

`LockFaceSetup.exe` 使用 Go、Wails v2 和 Vue 3，负责部署与卸载，不参与运行时认证或命名管道通信。构建命令与资源白名单只在 [`BUILD.md`](../BUILD.md) 维护。

## 代码入口

| 位置 | 责任 |
|---|---|
| `app.go` | 安装/卸载编排、进度事件和目录选择 |
| `internal/extract.go` | 载荷枚举、文件提取、模型校验及卸载静态清单 |
| `internal/com.go` | COM 注册/注销和目录 ACL |
| `internal/security.go` | LockFace 注册表键 ACL |
| `internal/scm.go` | Windows 服务控制 |
| `internal/config.go` | 默认配置创建与选择性迁移 |
| `frontend/src/App.vue` | 安装与卸载界面 |
| `resources/` | 构建时载荷镜像，不是资源清单事实源 |

`frontend/wailsjs/` 是生成绑定，不手工编辑。

## 隐私说明阅读确认

随包文书以 [`privacy-notice.txt`](../legal/privacy-notice.txt) 为准。安装界面要求阅读确认，`internal/consent.go` 的 `ConsentVersion` 与文书版本由测试校验一致；历史 `RecordConsent` 名称及 HKLM 注册表值名保留，但它们只记录安装时的阅读确认，不授权采集人脸，也不代替各 Windows 用户在 Console 中作出的单独授权。界面确认及人脸授权均不包含接受免责或放弃法定权利。

## 安装安全不变量

安装流程须保证：

- 在停止或移除现有服务前，先校验完整嵌入载荷；损坏载荷不能破坏可用安装。
- 可执行文件写入前保护目标目录 ACL，启动 SYSTEM 服务前再次确认模型已落盘且校验通过。
- 安装器拥有的程序数据目录限制为 SYSTEM 和 Administrators；V7 `users.dat` 密码由 SYSTEM 服务使用用户作用域 DPAPI 保护；文件 ACL 继续保护账号、人脸特征和文件完整性。V6 主库和每日备份由新版服务启动时升级。
- 服务更新前清理仍占用文件的旧服务、worker 和安装完成页控制台进程，避免覆盖失败。

调整 `app.go` 顺序或 ACL 时，检查 `internal/com_test.go` 及安装器集成测试。安装目录默认也作为 `DataPath`；路径解析规则见 [`DEVELOPMENT.md`](../../DEVELOPMENT.md#sec-paths)。

## 载荷机制

完整安装器只嵌入 `payload.zip`；压缩结构、资源白名单与同步步骤见 [`BUILD.md`](../BUILD.md)。`resources/` 是 gitignore 目录，不能据它的当前内容推断正式载荷。

完整安装器从嵌入资源发现待卸载文件；slim 卸载器不含资源包，使用由 `internal/payload_manifest.txt` 派生的静态清单（`currentRootPayloadFiles`，见 `payload_manifest.go`；slim 构建同样嵌入该 manifest）和 `requiredModels`。载荷白名单的单一事实源与各消费方见 [`BUILD.md`](../BUILD.md#资源白名单)，增加载荷文件时测试会强制 zip、卸载清单与 manifest 三方一致。

提取器会校验模型哈希，并按相对路径枚举文件。调整资源目录布局时，同时检查提取、安装后校验和卸载清单。模型固定标识与资源白名单以代码和 BUILD 指南为准，不在此重复。

## 卸载语义

卸载停止并删除服务、注销 COM、清除安装器拥有的程序文件和用户数据，并删除 FaceLogin/LockFace 注册表键（含更名前旧键残留）。注册表清理同时移除 Console 同意门写入的每用户记录（`HKCU\SOFTWARE\LockFace`，PRIVACY.txt 7.6 的清理边界）：当前用户与 HKEY_USERS 下已挂载的各配置单元逐一清理，未登录用户的 hive 不挂载、不处理，单个 hive 失败仅告警。卸载器会尽力结束残留进程，使文件可立即删除；失败时使用经验证的重启删除队列。

根目录的 `camera_tune.state` 是运行时曝光缓存，不属于安装载荷。完整卸载通过独立的运行数据清单删除它，完整与 slim 路径一致；升级清理程序文件时保留。即使之前卸载只留下该缓存，后续卸载也能识别并清理目录。

删除操作只针对已知载荷和明确的数据路径，不对安装目录执行任意递归清除。未知文件会保留，并报告卸载未完全成功；安装目录仅在清空后移除。slim 卸载器通过分离的隐藏进程延迟删除自身。

## 验证

改动 Go 安装器后，在 `installer/LockFaceSetup/` 运行 `go test ./...`；卸载改动再运行 `go test -tags slim ./...`，载荷解包测试只参与完整构建。`internal` 回归覆盖曝光缓存清理、升级保留及未知文件保护。改动 Vue 前端后，在 `frontend/` 运行 `npm run build`。安装包构建和资源核对见 [`BUILD.md`](../BUILD.md)。
