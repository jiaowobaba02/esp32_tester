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
- **收藏**: 答题页顶栏 ☆收藏/★已藏 → 菜单"收藏"查看 (favorites 分区, 最多 511 题,
  列表每页 5 条, 底部 [上页][页码][下页] 翻页)
- **薄弱点**: 答错自动记录 (weakness 分区 9×32KB) → 菜单"薄弱点"选科 →
  错题列表 (每页 4 条, 左/右半屏翻页) + AI 分析总结 (全屏可翻页, 底部可切回错题列表)
- **长文本翻页**: 题目全文 / 解析 / 薄弱点总结超过一屏时进入全屏阅读页,
  **左半屏 = 上一页, 右半屏 = 下一页**, 边界点击退出 (长按亦可返回)
- **主题**: 明亮/护眼/夜间 (设置页切换, NVS 保存)
- **亮度**: LEDC PWM 背光 1-100% (设置页左减右加, NVS 保存)
- **年级**: 高一/高二/高三 (设置页切换, AI 出题按年级适配)
- **设置**: 扫描 WiFi (中文 SSID 支持) → 软键盘输密码 (数字/符号/大小写) → 自动连接保存
- **API Key**: WiFi 连上后浏览器访问 `http://IP:8080` 粘贴保存 (NVS)
- **顶栏**: 各页显示"长按返回"提示 + IP (5x7 小字)
- **触摸**: 点按操作, 长按 900ms = 返回
- **BOOT 键**: 全程兜底 (短按/长按)

## 使用流程

1. 上电 → 菜单页
2. 点科目 → 本地刷题; 点"AI 出题" → 选科目 → AI 生成
3. 答题: 点选项直接判分; 长题目点题目区看全文, 答完点解析区看完整解析 (均可翻页)
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
main/font_cn.c       16x16 中文字库 (GB2312 7445 字)
main/ascii16.c       14x16 雅黑 ASCII 字库
main/questions.c     本地题库 (100 题, 每科 10 题)
main/ai_quiz.c       DeepSeek API 出题 + 薄弱点分析
main/fav.c           收藏 (favorites 分区, 511 题)
main/weak.c          薄弱点 (weakness 分区, 9×32KB)
partitions.csv       自定义分区表 (factory 2MB + weakness + favorites)
sdkconfig.defaults   TLS insecure + 大任务栈 + flash 4MB
```

## 已知限制

- ESP32 仅支持 2.4GHz WiFi (5GHz 热点扫不到)
- DeepSeek 证书 TrustAsia 不在 esp 证书包, 已跳过 TLS 验证 (个人使用)
- AI 出题需等待 10-60 秒 (DeepSeek 生成时间)
- API Key 明文存 NVS (个人设备可接受)

## 调试

- 串口 115200: 日志 (tap 坐标 / wifi 状态 / ai 结果)
- 触摸坐标: 校准数据 (5 点: 左上 65,40 / 左下 64,230 / 右下 416,220 / 中心 242,144)

> 注：该项目完全由Deepseek-v4-flash-0731 编写，总共耗费8.39美元，我不推荐该代码商用，除非你能承受其不稳定性
