# OKSwitch

OKSwitch 是基于TVBox开发的 Nintendo Switch 版本，须在大气层系统中运行，起因是我的 switch 一直连着显示屏，有时候想看点视频还得切换HDMI设备，有点麻烦，干脆让switch也拥有电视盒子的功能吧。
用过TVBox的应该都知道，它提供了海量的影视来源搜索（需自行配置订阅源）、然后我自己平时也喜欢看看体育直播，所以加入了咪咕及 CCTV 直播。最后是 YouTube 页面，为了它我也在软件中加入了梯子功能，目前支持的机场如下：
Trojan：普通 TCP/TLS
VLESS：TCP + TLS
SS：AES-128-GCM、AES-256-GCM、ChaCha20-IETF-Poly1305
SSR：`origin + plain`，配合 AES-128-CFB 或 AES-256-CFB
VMess：AEAD TCP，可选 TLS
AnyTLS：Clash YAML 中的内联节点。
挂上梯子还有一个好处是可以在线更新，毕竟服务器还是太贵啦。

## 安装

从 [Releases](https://github.com/LuffyZhu/OKSwitch/releases) 下载 `OKSwitch.nro`，放入 SD 卡的 `/switch/OKSwitch.nro`。如使用指向这个路径的桌面转发器，更新 NRO 后无需重新安装转发器。

首次运行后按 **−** 打开设置，在「视频源配置地址」输入自己的配置链接。仓库和安装包不预置订阅地址。YouTube 账号、代理和机场订阅也在设置页配置；仅 YouTube 和在线更新使用该代理，其他影视和直播请求仍按各自网络设置访问。

## 在线更新

进入「− 设置 → 在线更新」，OKSwitch 会读取本仓库的 [`update.json`](update.json)，通过当前 YouTube 代理下载 Release 安装包，验证文件大小与 SHA-256 后替换 SD 卡上的 NRO。退出并重新打开后生效。直连网络下，更新请求也直连。当前支持 `/switch/OKSwitch.nro` 与 `/switch/OKSwitch/OKSwitch.nro` 两种位置。

首次安装及此版本的更新入口需要手动替换 NRO；之后可在应用内更新。发布者的步骤见 [发布说明](docs/RELEASING.md)。

## 构建

需要 devkitPro、libnx、Switch 移植库及 `Makefile` 中列出的依赖。在 MSYS2 devkitPro 环境运行 `make -j4`，生成 `OKSwitch.nro`。构建所需的本机依赖目录 `.deps/` 不随仓库发布。

本项目不包含用户 Cookie、账号凭据、代理订阅或抓包文件。第三方图像素材的说明见 `docs/third-party/`。

## 赞赏 ☕

如果 OKSwitch 对你有帮助，欢迎自愿赞赏；不影响任何功能的使用。

| 微信赞赏 | 支付宝赞赏 |
| :---: | :---: |
| <img src="https://raw.githubusercontent.com/LuffyZhu/MusicBox/main/docs/images/wechat.png" alt="微信赞赏二维码" width="220" /> | <img src="https://raw.githubusercontent.com/LuffyZhu/MusicBox/main/docs/images/alipay.jpg" alt="支付宝赞赏二维码" width="220" /> |
