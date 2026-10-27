# `users.dat` 持久化契约

本文定义 LockFace 凭据数据库 V8 契约。实现事实源是 [`storage/credential_store.h`](../../storage/credential_store.h) 和 [`storage/credential_store.cpp`](../../storage/credential_store.cpp)。不要根据本文单独重写解析器；修改格式时必须同步所有消费者。

## 位置与安全属性

- 文件位置：`<DataPath>\data\users.dat`
- 默认生产位置：`C:\Program Files\LockFace\data\users.dat`
- 模板与身份：父服务使用 SYSTEM 用户作用域 DPAPI 加密整库内层；主库、备份和新临时文件只含密文及最少 header。注册端仅通过服务操作数据库，不读取既有模板。
- 密码：SYSTEM 服务使用用户作用域 DPAPI（不设置 `CRYPTPROTECT_LOCAL_MACHINE`）加密。普通用户仅拿到文件不能直接解密；不承诺抵抗 SYSTEM 或整盘离线攻击
- `data\` 目录及其文件 DACL 不向普通用户授权：安装时由安装器 `SetDataDirectoryACL` 设置 SYSTEM+Administrators 权限；服务每次启动通过 `EnsureSecureDirectory` 创建/修复并复核整个数据子树（含 `%ProgramData%` 回退、主库、备份、临时文件及显式文件 ACE）。低权限所有者和重解析点直接拒绝；可信所有者的权限无法收紧也拒绝启动。开发构建目录例外见服务模块文档。DACL 与整库 DPAPI 共同保护账号、人脸特征和文件完整性；密码内层仍有独立的 SYSTEM 身份 DPAPI 保护；Users 对安装目录其余部分仍保持只读（DLL 加载所需）
- 文本：Windows `wchar_t[]`，UTF-16LE，不写结尾 `NUL`
- 数值：当前仅支持 Windows x64 本机产生的定长整数/IEEE 754 `float`

不得记录密码、DPAPI 密文或完整认证消息。密码解密后每条退出路径都要清零。

## V8 外层与保护范围

外层固定 20 字节，依次为五个 uint32：`magic=0x474F4C46`、`version=8`、`accountCount`、`recognizerTag=0x31434653`、`ciphertextBytes`，随后是恰好该长度的 DPAPI blob。最大文件大小 1 MiB。解密后的内容首先重复相同的 20 字节 header，再跟随下述 Account 记录。外层与受保护 header 必须逐字节相同；截断、尾随数据、损坏密文、异常长度和模型不匹配均拒绝。

DPAPI 可能容忍 blob 后多余字节，因此密文长度也放入受保护 header。保存时先探测相同长度明文的密文长度，再写入该字段重新保护；最终长度必须一致。数据和密钥不写入注册表，不设置 `CRYPTPROTECT_LOCAL_MACHINE`。不保证跨机器或重装后恢复，也不承诺抵抗 SYSTEM、整盘离线攻击或有权限攻击者重放完整旧文件。

外层仅供 Credential Provider 判断是否显示磁贴，不能作为认证依据。身份、标签、128-D 向量、角度和已有密码密文均在受保护内层。密码保留独立 DPAPI 层，全部认证门通过后才单独解密。

## V8 受保护内层布局

字符串长度为 UTF-16 code units，embedding 长度为 float 个数；密码长度和 ciphertextBytes 为字节数。

```text
[Header]
  magic:          uint32 = 0x474F4C46  // "FLOG"
  version:        uint32 = 8           // SFace + SYSTEM DPAPI envelope
  accountCount:   uint32
  recognizerTag:  uint32 = 0x31434653  // "SFC1", pinned SFace 2021dec
  ciphertextBytes:uint32             // 与外层及实际密文长度一致

[Account] × accountCount
  usernameLen:    uint32
  username:       wchar_t[usernameLen]
  upnLen:         uint32
  upn:            wchar_t[upnLen]
  sidLen:         uint32
  sid:            wchar_t[sidLen]
  passwordLen:    uint32               // bytes
  passwordBlob:   uint8_t[passwordLen] // SYSTEM user-scope DPAPI blob (length >= 2)
  faceCount:      uint32

  [Face] × faceCount
    faceId:       uint32
    labelLen:     uint32
    label:        wchar_t[labelLen]
    embeddingLen: uint32               // float count
    embedding:    float[embeddingLen]
    nominalYaw:   float                // V5：角度槽标称 yaw（度）       ← V5
    nominalPitch: float                // V5：角度槽标称 pitch（度）     ← V5
```

`nominalYaw`/`nominalPitch` 是录入时的**姿态目标**（`EnrollmentWizard` 的 `kAngleTargets`：正面 0/0、左转 +30/0、右转 −30/0），不是实测均值——固定目标保证三个角度槽的 ±25° 学习锥（[渐进式学习方案](../work/in-progress/progressive-learning-v2.md) §3）互不重叠，即使用户转角不足。合法值域 ±90°；无效哨兵 `1000.0f` 表示"无角度信息"，未传角度的内存构造携带它；V4 数据已停用，不会迁入新库，序列化按原值往返（锥门对其放行）。

当前写入上限：

- `kMaxUsers = 5`
- `kMaxFacesPerUser = 3`
- `faceId >= 1`，账号内唯一
- 新增脸复用最小空闲 ID
- 当前 embedding 为 128 维 SFace

读路径对 `faceCount` 接受 1..16，embedding 长度仅接受 128。不要把读路径宽容度误认为新数据写入上限。

## 版本策略

正常读写只接受 V8/SFC1；未迁移 V6/V7 不能参与认证。SYSTEM 服务启动先独立迁移 `users.dat.bak`，再迁移主库：V6 密码重新保护为 SYSTEM 用户作用域；V7 保持已有密码 blob；两者保留 SFace 身份、人脸、标签和角度，无需重录。V8 幂等保留。V4/V5 不导入旧模型人脸或密码，替换为空 V8，要求重录。普通只读加载 V4/V5 仍返回空库，不改文件。

缺失文件视为空库；访问、解析、解密或提交失败阻止服务启动，不覆盖为空库，不降级明文。迁移只在内存生成明文，新临时文件写入的也是 V8 密文，再同目录原子替换；中断后下次启动继续。两份数据库完成迁移后清理 `users.dat.tmp.*` / `users.dat.bak.tmp.*` 旧临时文件，清理失败也拒绝启动。已复制的外部旧库和磁盘快照无法追溯保护。产品组件须一起升级，旧版本不能读取 V8。

父服务启动和重载校验后释放明文，只保留加密字节快照和账户数量。每轮认证在原 5 秒预算内恢复一次模板，全帧复用，所有返回路径清零；不增加公共 IPC 往返，不在预载模型时恢复模板。管理请求也短暂解密并在操作后清理。当前识别器的渐进式学习仍未验证、保持关闭；重新开放学习前必须重新核查其样本与历史模板驻留边界。

注册及密码更新必须调用已验证 SYSTEM 服务的 `PROTECT_PASSWORD` 加密专用 IPC，不能在注册进程内加密。服务不提供通用解密接口。升级不会保护外部已复制的旧密文；疑似泄露时必须更换对应账号密码。

## 账号与人脸操作语义

- `AddFace`：账号不存在则创建；存在则追加人脸，不覆盖已存密码。
- `DeleteFace`：删除指定脸；若账号不再有脸则移除整个账号。
- `ClearFacesForAccount`：清空该账号人脸但保留身份/密码，供多角度重录替换。
- `ClearAllFaces` / `DeleteUserBySid`：删除整个账号。
- `UpdateAccountIdentity`：更新身份与密码，保留全部人脸。
- `RenameFace`：只改标签。
- 所有写操作都必须显式调用 `SaveDatabase()` 才落盘。

产品中的单脸删除、清空账户与撤回同意统一由父服务执行 `DeletePersistedFace`，在服务存储锁内重新读取主库、清理旧 `.bak` 及 `users.dat.tmp.*` / `users.dat.bak.tmp.*` 中断写入残留，再原子保存主库；零 faceId 删除整个账户（含密码密文），正数只删除指定脸，最后一张脸删除时移除账户。目标已不存在视为成功，以支持丢失响应后的安全重试。清理或保存失败不得返回完成；主库提交失败时内存保留本次删除，并使旧密文缓存失效；结束操作清理明文后认证拒绝恢复旧快照，直到显式重载或重试。重试从磁盘重新执行。服务在操作前停止并回收学习线程，销毁待处理样本和历史模板，操作后重新创建学习器（所有账户的学习统计随之重置）。

跨进程读、写、迁移与每日备份使用同目录短生命周期 `users.dat.lock` 文件互斥，句柄关闭或进程退出即自动删除。加载时记录主库字节快照；保存前在文件锁内复核，任何变化都拒绝旧快照覆盖，调用方须重新加载。未加载的存储对象只允许创建不存在的主库。每日备份在同一文件锁内从当前主库复制，旧快照不能生成备份。普通删除、覆盖重录、身份或密码更新后的保存也先清理恢复文件；重命名及正常学习不主动清理每日备份。旧外部副本、系统备份和磁盘快照不在软件删除能力范围内，不保证物理介质不可恢复。磁盘布局为 V8/SFC1。

多角度注册每个角度独立保存一条 embedding 并写入该角度的标称 yaw/pitch；禁止跨角度平均。

## 匹配语义

1. 只比较相同 embedding 维度。
2. 账号内取所有脸的最小距离作为账号距离。
3. 账号间选 best / second-best。
4. best 必须小于维度对应阈值。
5. 多账号时执行最佳/次佳比门控；单账号不执行 ratio 拒绝。
6. 成功后才解密密码；使用后立即清零。

128 维 SFace 试用阈值由 `EmbeddingThresholdForDim` 再次收口。具体数值和标定依据见 [历史阈值标定](../work/completed/threshold-calibration.md) 与 [`docs/modules/face-service.md`](../modules/face-service.md)。

## 消费者与兼容风险

- `LockFaceService`：完整加载、匹配、保存
- `LockFaceConsole`：通过经验证的服务请求注册、追加、删除、重命名与身份更新；仅获得本人账户元数据，不读写磁盘库
- Credential Provider：只读 header（仅接受 V8/SFC1）判断是否显示磁贴

## 修改检查表

格式或语义变化时至少同步检查：

- `storage/credential_store.h/cpp`
- `credential_provider/LockFaceProvider.cpp` 的 header 读取
- `enrollment_app/EnrollmentWizard.*`
- 安装/升级/卸载是否保留或清除用户数据
- 本文和 `DEVELOPMENT.md`

验证至少覆盖：空数据库、不支持的版本拒绝、V4/V5 停用、V8 模型标识和标称角往返、V6/V7 主库和备份升级、非 SYSTEM 升级拒绝、多脸、密码 blob 长度、上限、截断/异常长度、删除最后一张脸、保存后重载、不同 embedding 维度，以及 DPAPI 解密失败。
