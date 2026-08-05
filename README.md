# esp32_tester 刷题机 (ESP32 + 4.3寸 480x272 触摸屏)

EYA ETSP32 板 + ST6201 屏的完整刷题机：本地题库 + DeepSeek AI 出题。

## 硬件配置 (已验证)

- **主控**: ESP32-WROOM-32E (ETSP32 板)
- **屏幕**: 4.3寸 480x272 ST6201, SPI 4线
- **触摸**: GT911 (I2C)
- **接口**: 板内走线 (无需飞线, 除背光)

### GPIO 映射 (官方 EYA 配置)

```
屏幕 SPI:  SCK=GPIO23  MOSI=GPIO19  CS=GPIO22  DC=GPIO14  RST=GPIO12
背光:      GPIO2 (板上 BCKL, 高电平开)  +  GPIO32 (飞线备份, 高电平开)
触摸 GT911: SDA=GPIO18  SCL=GPIO16  RST=GPIO4  INT=GPIO17
BOOT 键:   GPIO0 (低电平按下, 兜底导航)
```

### 屏幕驱动要点 (踩坑记录)

1. **SPI mode 0** (CPOL=0, CPHA=0), 26MHz, SPI2_HOST + DMA
   (GPIO matrix 路由上限 26.6MHz; 40MHz 会被 ESP32 驱动拒绝)
2. **像素大端** (高字节先) — 小端会导致 R→B/G→R/B→G 通道错乱
3. **RAMWR 像素流 CS 必须全程拉低** — 每行拉高会花屏 (软件 CS 控制)
4. 初始化序列必须用 EYA 官方版 (0x36=0xC0, 0x41=0x03, BOE Gamma)
5. 背光 GPIO2 高电平开 (与 8080 版 README 不同 — 本板实测)

### 触摸要点

- GT911 地址 0x14 (7bit), 坐标上限出厂已配 480x272 (读 0x8048/0x804A)
- 状态寄存器 0x814E: bit7=有新数据, 低4位=点数; **读后需写 0 清除**
- 坐标寄存器 0x8150 起: **每点 4 字节小端** (Xlo, Xhi, Ylo, Yhi)
- 硬件 I2C 400kHz 稳定; 软件 I2C 不工作 (GT911 clock stretching)
- 触摸坐标直接映射 (无交换/镜像, 校准验证过)

## 功能

- **本地刷题**: 9 科菜单 → 题目 → 点选项判分 → 解析翻页 (题库 100 题, 每科 10 题)
- **AI 出题**: 菜单 → AI 出题 → 选科目 → DeepSeek 按年级生成 (答完自动再出一题, 失败可重试)
- **收藏**: 答题页顶栏 ☆收藏/★已藏 (选择/填空/解答题均可收藏; 收藏题查看页点 ★ 即取消并返回列表) →
  菜单"收藏"查看 (favorites 分区, 最多 256 题, **最新收藏在前**, 同一题不可重复收藏,
  列表每页 5 条, 每行右侧 [移除] 直接删藏, 底部 [AI 分析][上页][页码][下页])
  → **AI 分析**: 勾选 1-5 道收藏题 → AI 逐题点评 (全屏可翻页)
- **顶栏时间**: WiFi 联网后 SNTP 自动校时 (ntp.aliyun.com), 顶栏显示 月-日 时:分
- **薄弱点**: 答错自动记录 (weakness 分区 9×128KB) → 菜单"薄弱点"选科 →
  错题列表 (每页 4 条, 左/右半屏翻页) + **逐题选择分析** (勾选 1-5 题 →
  AI 按"题N"逐题点评知识点/错因/建议, 全屏可翻页)
- **知识库**: 菜单 → 知识库 → 选科目 → 主题池 (每科 7 槽: 预置 + 自定义)
  → 生成该主题核心知识点 (约 3000 字, 缓存可反复查看) → 全屏翻页
  - 列表可翻页 (每页 5 行), 已生成主题右侧 [删除] 可删减
  - **[自定义主题]**: 软键盘输入拼音/英文 (如 PCR yuanli) → AI 自动转中文标题
    (PCR原理) 后生成; 槽满提示先删除
  - 容量: 主题列表页显示已用槽数/剩余 KB
- **长文本翻页**: 题目全文 / 解析 / 薄弱点总结 / 知识库超过一屏时进入全屏阅读页,
  **左半屏 = 上一页, 右半屏 = 下一页**, 边界点击退出 (长按亦可返回)
- **自适应分页**: 答题页题干超过 4 行自动分页, 选项文字超过 2 行自动分页,
  控件行 [−][页码][+] 或点题目区翻页, 无需跳全屏
- **主题**: 明亮/护眼/夜间 (设置页切换, NVS 保存)
- **亮度**: LEDC PWM 背光 1-100% (设置页左减右加, NVS 保存)
- **年级**: 高一/高二/高三 (设置页切换, AI 出题按年级适配)
- **设置**: 扫描 WiFi (中文 SSID 支持) → 软键盘输密码 (数字/符号/大小写) → 自动连接保存
- **存储显示**: 设置页底部显示知识库剩余空间 + 收藏占用槽数
- **API Key**: WiFi 连上后浏览器访问 `http://IP:8080` 粘贴保存 (NVS)
- **顶栏**: 右上角返回键图形 + IP 小字; 答题页顶栏长按/点右上角返回 (收藏题回收藏夹)
- **触摸**: 点按操作, 长按 900ms = 返回 (答题顶栏/解析页/键盘页均可长按返回)
- **BOOT 键**: 全程兜底 (短按/长按)

## 使用流程

1. 上电 → 菜单页
2. 点科目 → 本地刷题; 点"AI 出题" → 选科目 → AI 生成
3. 答题: 点选项直接判分; 长题目点题目区看全文, 答完点解析区看完整解析 (均可翻页);
   收藏夹点行查看, 点行右侧 [移除] 删藏
4. 设置: 右下角"设置" → 扫描 WiFi / 手动输入 → 密码键盘 → 完成自动连接
5. API Key: WiFi 连上后菜单左下角显示 IP, 浏览器打开 `http://IP:8080`

## 构建烧录

### 1. 安装 ESP-IDF (首次, Ubuntu/Debian 示例)

```bash
sudo apt install git wget flex bison gperf python3 python3-venv \
     python3-pip cmake ninja-build ccache libffi-dev libssl-dev dfu-util
git clone -b v5.5 --recursive https://github.com/espressif/esp-idf.git ~/esp-idf
cd ~/esp-idf
./install.sh esp32
. ./export.sh
```

### 2. 编译

```bash
cd esp32_tester  # 本工程目录
. $IDF_PATH/export.sh        # 新终端需先执行
idf.py set-target esp32      # 首次或更换芯片时
idf.py build
```

### 3. 烧录与串口监视

```bash
idf.py -p /dev/ttyUSB0 flash
idf.py -p /dev/ttyUSB0 monitor
```

注意: 工程使用自定义分区表 (factory 2MB + weakness + favorites) 和
sdkconfig.defaults (TLS 跳过证书验证 / 主任务栈 16KB / flash 4MB),
首次构建自动生效, 无需手动配置。

## 文件结构

```
main/main.c          主程序 (驱动 + UI + 状态机)
main/gfx_driver.c    ST6201 SPI 驱动 (26MHz, 大端像素, 连续事务模式)
main/ui_renderer.c   绘图层 (2bit 灰度字库解包 + Alpha 混合 + 脏区)
main/font_cn.c       16x16 中文字库 (7445 汉字, unicode 有序二分查找)
main/cn_gray.c       16px 微软雅黑灰度 (2bit/像素, 4 级抗锯齿)
main/ascii_gray*.c   Arial 灰度字库 16/24/32px (2bit 打包)
main/sym_gray*.c     特殊符号字库 110 个 (上标/下标/化学/希腊字母/全角数字等)
main/questions.c     本地题库 (100 题, 每科 10 题)
main/ai_quiz.c       DeepSeek API 出题 + 薄弱点分析 + 知识库生成 + 拼音转中文
main/fav.c           收藏 (favorites 分区 256 槽, 标记删除 + 槽位映射缓存)
main/weak.c          薄弱点 (weakness 分区, 9×128KB) + 知识库 (每科 7 槽×16KB)
partitions.csv       自定义分区表 (factory 2MB + weakness 1152KB + favorites 256KB)
tools/               字库生成脚本 (gen_font*.py) + flash 检查脚本 (inspect_*.py)
sdkconfig.defaults   TLS insecure + 大任务栈 + flash 4MB
```

## 直接烧录固件 (免编译)

发布包的 `firmware/` 目录含编译好的固件, 用 esptool 直接烧录:

```bash
# 方式 1: 用 ESP-IDF (推荐)
cd esp32_tester
. $IDF_PATH/export.sh
python -m esptool --chip esp32 -p /dev/ttyUSB0 -b 460800 \
  --before default_reset --after hard_reset write_flash --flash_mode dio \
  --flash_freq 40m --flash_size 4MB \
  0x1000 firmware/bootloader.bin \
  0x8000 firmware/partition-table.bin \
  0x10000 firmware/st6201_spi_lcd.bin

# 方式 2: 任意 Python 环境 (pip install esptool)
esptool --chip esp32 -p /dev/ttyUSB0 -b 460800 \
  --before default_reset --after hard_reset write_flash --flash_mode dio \
  --flash_freq 40m --flash_size 4MB \
  0x1000 firmware/bootloader.bin \
  0x8000 firmware/partition-table.bin \
  0x10000 firmware/st6201_spi_lcd.bin
```

烧录后上电: 设置 → 扫描 WiFi → 输入密码; 浏览器打开 http://IP:8080 粘贴 DeepSeek
API Key, 即可使用全部功能。

## 已知限制

- ESP32 仅支持 2.4GHz WiFi (5GHz 热点扫不到)
- DeepSeek 证书 TrustAsia 不在 esp 证书包, 已跳过 TLS 验证 (个人使用)
- AI 出题需等待 10-60 秒, 知识库生成约 1-3 分钟 (DeepSeek 生成时间)
- 薄弱点逐题分析每次最多 5 题 (保证输出完整不截断)
- API Key 明文存 NVS (个人设备可接受)
- v5 分区升级: 首次启动自动迁移收藏/错题 (约 6 秒), 旧知识库需重新生成

## 调试

- 串口 115200: 日志 (tap 坐标 / wifi 状态 / ai 结果)
- 触摸坐标: 校准数据 (5 点: 左上 65,40 / 左下 64,230 / 右下 416,220 / 中心 242,144)

>注：本项目完全由Deepseek-v4-flash-0731生成，总花费12.57$，我不建议任何人商用，除非你能承担风险。
