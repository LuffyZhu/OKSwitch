# 发布 OKSwitch 更新

1. 修改 `Makefile` 中的 `APP_VERSION` 和 `source/net/OnlineUpdater.cpp` 中的 `currentVersion`，保持一致。
2. 构建 `OKSwitch.nro`，计算文件大小和 SHA-256。
3. 在 GitHub 创建 `v<版本号>` Release，上传 `OKSwitch.nro`。先确认 Release 下载地址可访问。
4. 更新根目录 `update.json` 的 `version`、`url`、`size`、`sha256`，再推送到 `main`。务必最后推送清单，避免用户下载到尚未上传的版本。
5. 在真机从上一个版本检查并安装更新，确认重启后版本正确。仅依靠模拟器无法验证 SD 卡上 NRO 的替换行为。

在线更新下载时使用当前 YouTube 代理或直连，清单也走相同代理。更新文件会先保存为 `.download` 并校验；旧文件在替换时暂存为 `.backup`。
