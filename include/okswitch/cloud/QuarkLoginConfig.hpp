#pragma once
#include <string>
namespace okswitch::cloud::loginConfig {
inline const std::string tokenUrl="https://uop.quark.cn/cas/ajax/getTokenForQrcodeLogin?pr=ucpro&fr=pc&sys=win32&client_id=386&v=1.2&request_id=REQUEST";
inline const std::string pollUrl="https://uop.quark.cn/cas/ajax/getServiceTicketByQrcodeToken?pr=ucpro&fr=pc&sys=win32&client_id=386&v=1.2&request_id=REQUEST&token=TOKEN";
inline const std::string loginUrl="https://pan.quark.cn/desktop/account/login?pr=ucpro&fr=pc&sys=win32&st=TICKET";
inline const std::string flushUrl="https://drive-pc.quark.cn/1/clouddrive/auth/pc/flush?pr=ucpro&fr=pc&sys=win32&ve=3.1.8";
inline const std::string userAgent="Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) quark-cloud-drive/2.5.20 Chrome/100.0.4896.160 Electron/18.3.5.4-b478491100 Safari/537.36 Channel/pckk_other_ch";
inline const std::string qrUrl="https://su.quark.cn/4_eMHBJ?token=TOKEN&client_id=386&sch=pckk@other_ch&sve=3.1.8&ssb=weblogin&uc_param_str=&uc_biz_str=S:custom|OPT:SAREA@0|OPT:IMMERSIVE@1|OPT:BACK_BTN_STYLE@0";
}
