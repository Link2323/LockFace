# 界面语言

LockFace 内置简体中文和英文，同一安装包包含两套文案。安装器支持即时切换；注册控制台在右上角“外观与语言”菜单选择后，下次打开生效。语言是机器级偏好，影响本机所有账户的 LockFace 界面，与人脸处理同意记录分开。

## 选择与生效规则

`HKLM\SOFTWARE\LockFace\UiLanguage` 接受 `system`、`zh-CN`、`en`，缺失或无效值视为 `system`。显式选择优先；跟随系统时，当前进程的 Windows UI 语言属于中文则显示简体中文，其余回退英文。安装器和注册程序读取交互用户的 UI 语言；LogonUI 以 SYSTEM 身份运行，跟随该环境的 UI 语言。需要三端一致时应显式选择中文或英文。

该值只控制显示，不进入认证配置、IPC 或 `users.dat`。锁屏在显示文案时读取设置；已经显示的文字或已经创建的字段描述要到下次更新或重新加载组件才会变化。注册程序在页面启动时固定语言，切换设置不会改变当前页面及其文书语言。

安装器在操作期间禁用切换，并保留进程内快照，避免卸载删除注册表后改变语言。切换会清除本次安装阅读确认，重新加载对应语言的隐私说明，要求重新读到底部并确认。切换本身不撤回或授予已有的人脸处理同意。

## 文案维护入口

| 位置 | 编辑入口 | 使用方式 |
|---|---|---|
| 注册页面 | `enrollment_app/locales/zh-CN.json`、`enrollment_app/locales/en.json` | JS 使用 `lfT('key', parameters)`；HTML 使用 `data-i18n`，属性使用 `data-i18n-title`、`data-i18n-placeholder`、`data-i18n-aria-label` |
| 安装器 Vue | `installer/LockFaceSetup/frontend/src/locales/` 中同名 JSON | 从 `i18n.ts` 导入 `lfT`；模板使用 `{{ lfT('key') }}`，属性使用 Vue 绑定 |
| C++ 原生提示及服务错误显示 | `common/ui_messages.inc` | `{L"中文源文案", L"English"}`；原生界面通过 `facelogin::ui::Text` 取文案 |
| Go 安装进度、标题及错误 | 安装器 `internal/ui_messages.json` | 中文源文案为键，英文为值；通过 `internal.UIText`（包内为 `UIText`）取文案后再格式化 |
| 长文书 | `docs/legal/privacy-notice.txt` / `.en.txt`、`third_party/MODEL_LICENSES.txt` / `.en.txt` | 同步修订两种语言；隐私说明保持相同版本和条款编号 |

前端新文案使用有含义、稳定的键，同步补全两个 JSON 文件。例如：

```js
// zh-CN.json: "capture_count": "已采 {count} 帧"
// en.json:    "capture_count": "{count} frames captured"
caption.textContent = lfT('capture_count', {count: total});
```

包含变量的提示保存完整句子，让译文可调整语序。标记和样式留在代码中；用户输入继续通过 `textContent` 或既有 HTML 转义路径显示。更新后检查英文长度，特别是采集提示、隐私状态、弹窗和锁屏字段。

服务与 worker 传输的既有错误文本保持原值，Credential Provider 在显示边界翻译。新增服务/worker 中文错误也须补原生文案表。日志和第三方许可原文不按界面语言翻译。

`正面`、`左转`、`右转` 同时用作历史录入角度标签。`ANGLE_LABELS` 和 C++ 保存值保持原值，界面通过 `lfStoredLabel` 显示译名，缺失角度仍按原值判断。用户自定义名称不作通用翻译；切换语言不重写已有录入。

## 构建与验证

注册端 JSON 在 CMake 配置阶段嵌入页面，不新增运行期语言文件。原生文案编译进各自二进制，兼容 Credential Provider 的静态 CRT。安装器 JSON 由 Vue/Go 分别嵌入。

```powershell
node tools/i18n/check.cjs
node tools/enrollment_ui/capture_flow_test.cjs
cmake --build build --config Release --target LockFaceService LockFaceCredentialProvider LockFaceConsole --parallel 1
```

安装器目录执行 `go test ./...`、`go test -tags slim ./...`；其 `frontend/` 执行 `npm run build`，先运行文案检查。检查覆盖前端语言的键、命名参数、静态引用、原生错误译文、Go 格式化参数及隐私版本与条款编号。跨语言常量测试同时核对中英文采集角度和每账户人脸数量上限。

英文文书由 `scripts/stage_licenses.ps1` 暂存为 `PRIVACY.en.txt`、`MODEL_LICENSES.en.txt`，与中文原件一起进入载荷白名单和完整/slim 安装器的前端资源。同意版本仍从中文规范源 `PRIVACY.txt` 读取；显示时按界面语言选择对应文书，第三方许可原文保持原样。

浏览器可用宿主替身验证排版和阅读流程；发布前仍需管理员 GUI、摄像头和 LogonUI 实测，特别是英文锁屏失败提示的截断与重试。
