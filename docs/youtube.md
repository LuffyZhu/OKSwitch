# YouTube in OKSwitch

From the home screen, press L twice to reach YouTube. Guest search and playback do not need a Google account. Press Y to search. After signing in, the home page shows recent uploads from subscribed channels and up to three locally watched videos. Press X to open subscribed channels; choose a channel to see its recent uploads. The left menu also contains local watch history.

If the Switch network cannot connect to YouTube directly, press − and set **YouTube 网络代理** to an HTTP or SOCKS5 proxy reachable from the Switch, such as `http://192.168.1.10:7890` or `socks5h://192.168.1.10:7890`. The setting applies to YouTube search, pairing, subscription requests, and video downloads only. An empty value uses the direct connection.

机场订阅也可以在 Switch 内使用：按 − 进入设置，选择 **YouTube 机场订阅**。按 A 用 Switch 键盘输入完整 HTTPS 订阅地址，或按 Y 显示局域网二维码；让手机连接同一网络，扫码后在手机浏览器粘贴地址并发送。二维码页面只在 Switch 等待输入时开放，按 B 取消。OKSwitch 会读取节点列表；选中节点按 A 启用。软件在 Switch 内启动仅监听本机的 SOCKS5 转接，YouTube 搜索、登录、订阅和视频都使用该节点，不需要电脑常驻代理。优先选标为「校验证书」的节点；标为「不校验证书」的节点会按订阅提供的配置连接。再次进入该设置可以刷新订阅或换节点；在该行按 X 清除。下次启动时会自动重新读取订阅并启用所选节点。

不想在 Switch 上输入长地址时，可以把仅含订阅 URL 一行的 UTF-8 文本文件复制到 SD 卡 `/switch/OKSwitch/youtube-subscription.txt`。首次启动时会读取这个文件，并优先启用可校验证书的节点。之后在设置页选定节点会保存选择；在设置页按 X 清除订阅后，这个文件不会被自动再次导入。

在设置页输入或选择后的订阅地址保存在 SD 卡的 `/switch/OKSwitch/settings.json` 中，内含私人令牌；上述导入文件同样私密。不要分享这些文件。订阅令牌不会编入 NRO。当前支持普通 TCP/TLS Trojan、VLESS TCP+TLS、SS 的 aes-128-gcm / aes-256-gcm / chacha20-ietf-poly1305、SSR 的 origin+plain+aes-128-cfb / aes-256-cfb、VMess AEAD TCP（可选 TLS），以及 Clash YAML 中的 AnyTLS 节点。不支持的传输或加密参数会跳过该节点，例如 WebSocket、gRPC、Reality、SS 插件及大部分 SSR 混淆组合。AnyTLS 已用用户订阅中的节点在电脑上验证握手和 HTTP 响应，并通过 Switch 构建；Switch 真机尚未验证。以上代理只用于 OKSwitch 的 YouTube 功能，不影响网盘、咪咕和系统的其他应用。

读取 Clash YAML 只是使用该订阅的配置格式，不需要安装或运行 Clash。AnyTLS 会在同一 TLS 会话内顺序复用流，空闲会话最多保留 30 秒；服务器下发的填充策略经校验后用于后续新建会话。电脑端已验证同一会话连续打开两条流，并验证更新策略不再被服务器重复下发。Switch 真机的连接和播放速度仍需实测，不能据此保证与饿饭加速器专用客户端相同。

如果订阅服务对普通客户端返回空内容，OKSwitch 会用另一种常见客户端标识重试一次，并接受整份订阅使用 URL 安全 Base64 编码。若服务只返回套餐状态文字或没有节点，会显示具体提示；HTTP 200 本身不表示订阅有可连接节点。

To enable pairing-code sign-in and subscriptions:

也可使用网页 Cookie 登录：将 Chrome 的 YouTube 请求头 Cookie 值保存为 UTF-8 文本 `youtube-cookie.txt`，放在 SD 卡 `/switch/OKSwitch/youtube-cookie.txt`。按 − → YouTube 账号 → A，程序会向 YouTube 验证登录后读取网页订阅频道，无需 OAuth 客户端配置。每次启动后可在这里按 A 验证文件；过期时替换文件再验证。按 X 退出账号会删除 SD 卡 Cookie 文件。Cookie 是登录凭据，不要分享，也不会编入 NRO。网页方式目前显示订阅页和频道视频页首批返回内容，不包含后续分页。

1. In your own Google Cloud project, enable **YouTube Data API v3** and configure the OAuth consent screen. If the app is in testing mode, add your Google account as a test user.
2. Create an OAuth client of type **TVs and Limited Input devices**.
3. On the Switch SD card create `/switch/OKSwitch/youtube-oauth.json` with the client values:

   You can also copy the downloaded Google OAuth JSON file unchanged to that path;
   OKSwitch accepts its `installed` wrapper.

   ```json
   {
     "client_id": "YOUR_TV_CLIENT_ID.apps.googleusercontent.com",
     "client_secret": "YOUR_TV_CLIENT_SECRET"
   }
   ```

4. Restart OKSwitch. Press − from any screen, select **YouTube 账号**, then press A. Visit the displayed Google address on your phone and enter the displayed code. Do not send your client values or pairing code to anyone.

OKSwitch requests only `youtube.readonly`. It stores the refresh token at `/switch/OKSwitch/youtube-session.json` on the SD card so you do not have to pair every time. The file is not encrypted; treat it as private. Press X on the YouTube account row to sign out and remove it.

Playback chooses the highest available H.264/AAC MP4 pair, up to 1080p in TV mode and 720p in handheld mode. When the video offers only a combined 360p file, that remains the fallback. Some videos may lack directly usable H.264/AAC URLs. DRM videos are not supported. With OAuth pairing, OKSwitch first makes a read-only request to YouTube's undocumented browse API for account recommendations and watch history. It displays those results only if the response explicitly confirms a logged-in account. Otherwise the home page uses subscribed-channel uploads and the history page uses local records. Watch history and resume positions are saved locally to `/switch/OKSwitch/youtube-history.json`.


## 首页与节点选择（2026-10-23）

YouTube 页使用独立的深色左侧导航和三列横向封面，每屏六张卡片。第一列按左进入导航，右键回到内容；标题两行，选中后超长第二行滚动。A 打开，Y 搜索，X 切换订阅，− 设置。

已有机场订阅地址时，设置中的 A 直接下载节点列表并显示连接延迟。Y 通过局域网扫码更换订阅地址，X 清除。

进入首页时，如有有效网页 Cookie，会解析 YouTube 网页首页推荐；网页结果目前只加载首批数据。OAuth 登录时先试读 YouTube 非公开浏览接口；只有响应确认已登录账号且返回视频时才标为「账号推荐」。否则首页展示最多 12 个订阅频道最近上传的视频，并在前面放最多 3 个本机最近观看的视频。历史页同样先试读账号历史，失败时保留本机记录。

### SmartTube 参考核对

仅参考交互和协议流程，未复制其代码或素材。
- UI/设备码展示：https://github.com/yuliskov/SmartTube/blob/master/common/src/main/java/com/liskovsoft/smartyoutubetv2/common/app/presenters/YTSignInPresenter.java
- 授权接口：https://github.com/yuliskov/MediaServiceCore/blob/master/youtubeapi/src/main/java/com/liskovsoft/youtubeapi/auth/V2/AuthApi.java
- 客户端配置：https://github.com/yuliskov/MediaServiceCore/blob/master/youtubeapi/src/main/java/com/liskovsoft/youtubeapi/app/AppServiceCore.java

SmartTube 使用 YouTube 电视设备码接口、轮询令牌，并从电视客户端脚本解析客户端配置。这与 OKSwitch 已有的自有 Google OAuth 客户端配置不同。本次完成源码核对，未移植其动态电视客户端配置获取；免配置配对码登录仍未接通。

验证：编译通过。已有本地 Cookie 的只读首页请求返回 HTTP 200，但未识别登录，所以尚未实证当前账号的推荐列表；需有效 Cookie 和真机验证。


## 扫码导入 OAuth 客户端

Google 官方要求在 Cloud 控制台手动创建电视和输入受限设备客户端；该创建步骤不能由 OKSwitch 脚本代办。下载客户端 JSON 后，在 Switch 设置页选择 YouTube 账号、按 Y；用同一局域网中的手机扫描二维码，在临时网页中粘贴完整 JSON 并发送。Switch 校验 client_id/client_secret，保存到 `/switch/OKSwitch/youtube-oauth.json`，本次运行即可按 A 申请配对码。扫码服务只在二维码页打开时监听，随机 URL，提交一次后关闭。旧 Cookie 文件若还在，已导入的 OAuth 客户端登录优先于 Cookie。OAuth 首页展示订阅更新；YouTube 账号的推荐与观看历史不能用公开 Data API 读取。
