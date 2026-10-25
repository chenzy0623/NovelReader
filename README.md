# 小说阅读器 NovelReader

一个**单文件、免安装、零依赖**的 Windows 小说阅读器。用 C++17 + Win32 从零写成，
**530 KB** 一个 exe，双击就能用，不写注册表、不装运行库、不需要联网。

支持 **TXT / EPUB / PDF / DOCX / HTML / Markdown**，自动识别章节，全文搜索，
记忆阅读位置，自动下滑，书签，换字体，自定义配色，还能直接把网页小说抓成本地书。

![版本](https://img.shields.io/badge/version-1.2.0-blue)
![体积](https://img.shields.io/badge/size-530%20KB-brightgreen)
![平台](https://img.shields.io/badge/platform-Windows%2010%20%2F%2011-lightgrey)
![依赖](https://img.shields.io/badge/dependencies-none-success)
![许可](https://img.shields.io/badge/license-MIT-orange)

**作者**：[chenzy0623](https://github.com/chenzy0623)（第一作者） · xuanmo 的大肥鱼（第二作者）

---

## 下载与运行

到 [Releases](https://github.com/chenzy0623/NovelReader/releases) 下载最新版：

- **[NovelReader-1.2.0-win64.zip](https://github.com/chenzy0623/NovelReader/releases/download/v1.2.0/NovelReader-1.2.0-win64.zip)**（推荐，含使用说明和示例小说）
- [单个 NovelReader.exe](https://github.com/chenzy0623/NovelReader/releases/download/v1.2.0/NovelReader.exe)（只要程序本体）

解压任意目录，双击 `NovelReader.exe` 即可。**绿色版**：阅读进度存在 exe 同目录的
`NovelReader.ini`（目录不可写时自动改用 `%APPDATA%\NovelReader\`），删掉 ini 就等于恢复出厂。

第一次打开会看到书架页面；拖一个小说文件进去，或者按 `Ctrl+O` 选文件。

---

## 特性一览

| 特性 | 说明 |
|---|---|
| 章节自动划分 | 认「第X章 / 第X回 / 第X节 / 序章 / 楔子 / 番外 / Chapter N」等写法，上千章的表也能秒开 |
| 记忆阅读位置 | 关掉再开，接着上次那句往下读；每本书各记各的，书架上一眼看到进度百分比 |
| 全文搜索 | 关键词或章节名都能搜，`F3` 逐个跳，命中处高亮，左边列出「章节名 · 上下文片段」 |
| 自动下滑 | `F6` 开关，`F7/F8` 调速（1~10 级，3 秒/行 ~ 0.3 秒/行），手一碰键盘鼠标就自动停 |
| 书签 | `Ctrl+D` 添加，`Ctrl+B` 列表，正文左侧显示小米色书签条 |
| 字体 | 系统字体随便选，也能**导入 ttf/otf/ttc 字体文件**私有加载（不污染系统字体库） |
| 配色 | 日间 / 夜间 / 护眼绿 / 羊皮纸 / 高对比暗 五种预设，还能逐项自定义背景、正文、标题、选中、悬停色 |
| 多格式 | TXT、EPUB、PDF、DOCX、HTML、Markdown，以及任意扩展名的纯文本 |
| PDF 两种看法 | 默认抽文字重排（能搜索、能换字体）；`F9` 切「原始版式」，整页按原样渲染，图表、分栏、字体效果一点不走样 |
| 网页抓书 | `Ctrl+U` 输入目录页网址，自动找目录、逐章抓取，存成本地书 |
| 手机阅读 | 局域网 HTTP 服务，手机浏览器直接读（菜单位置：帮助 → 手机阅读） |
| 大文件 | 14.7 MB / 8794 章实测 0.8 秒载入，只渲染可视区域，滚动不掉帧 |
| 目录收起 | 点侧栏底部「‹ 收起目录」或 `Ctrl+T`，正文占满整屏 |
| 单实例 | 重复双击不会开出第二个窗口互相覆盖进度，而是把文件转发给已开着的那个 |

---

## 界面截图

主界面（TXT 大文件，左侧目录 + 正文）：

![主界面](https://github.com/chenzy0623/NovelReader/raw/main/screenshots/01-main.png)

全文搜索（127 处命中，带章节名和上下文）：

![搜索](https://github.com/chenzy0623/NovelReader/raw/main/screenshots/02-search.png)

夜间模式：

![夜间](https://github.com/chenzy0623/NovelReader/raw/main/screenshots/03-night.png)

自动下滑（状态栏显示档位）：

![自动下滑](https://github.com/chenzy0623/NovelReader/raw/main/screenshots/04-auto.png)

首页书架（书名 + 读到第几章 + 百分比）：

![书架](https://github.com/chenzy0623/NovelReader/raw/main/screenshots/06-shelf.png)

收起目录，正文占满宽度：

![收起目录](https://github.com/chenzy0623/NovelReader/raw/main/screenshots/07-fold.png)

书签（左侧书签条 + 书签列表）：

![书签](https://github.com/chenzy0623/NovelReader/raw/main/screenshots/10-bookmarks.png)

配色预设（护眼绿 / 高对比暗）：

<img src="https://github.com/chenzy0623/NovelReader/raw/main/screenshots/11-colors-green.png" width="49%"> <img src="https://github.com/chenzy0623/NovelReader/raw/main/screenshots/12-colors-dark.png" width="49%">

换字体（示例为楷体）：

![字体](https://github.com/chenzy0623/NovelReader/raw/main/screenshots/14-font.png)

PDF 文字重排（可搜索、可换字体、夜间模式）：

![PDF](https://github.com/chenzy0623/NovelReader/raw/main/screenshots/13-pdf.png)

PDF 原始版式（`F9`，整页按原样渲染，右下角显示页码）：

![PDF 原始版式](https://github.com/chenzy0623/NovelReader/raw/main/screenshots/13-pdf-layout.png)

EPUB：

![EPUB](https://github.com/chenzy0623/NovelReader/raw/main/screenshots/08-epub.png)

网页小说抓取：

![网页](https://github.com/chenzy0623/NovelReader/raw/main/screenshots/15-web.png)

---

## 支持的格式

| 格式 | 怎么读 | 说明 |
|---|---|---|
| `.txt` | 直接解码 | 自动识别 UTF-8 / GBK / GB18030 / UTF-16 LE、BE（带 BOM 或自动猜测） |
| `.epub` | 内建 zip 解压 + 解析 OPF | 按 `spine` 顺序读，章节顺序跟原书一致 |
| `.pdf` | 内建 PDF 解析器 + 系统 `Windows.Data.Pdf` 渲染 | 两种看法：默认抽出文字层**重新排版**（可搜索、换字体、夜间模式）；`F9` 切**原始版式**整页渲染，版式 100% 还原。扫描版/图片型 PDF 没有文字层，文字模式读不出内容，但原始版式照样能看 |
| `.docx` | 内建 zip 解压 + 读 `word/document.xml` | 只取段落文字，表格内容会按行拼进来；图片不显示 |
| `.html` `.htm` | 去标签取正文 | 解码实体（`&ldquo;` 等），跳过 script/style/head |
| `.md` | 去掉标记符号 | `#` 标题变章节名，列表/引用/加粗记号剥掉 |
| 任意扩展名 | 当纯文本 | 只要内容是文本就行 |

**不支持**：PDF 扫描件、加密 PDF、用对象流压缩（ObjStm）的 PDF、mobi / azw3 / chm、漫画压缩包。

---

## 快捷键

| 按键 | 功能 | 按键 | 功能 |
|---|---|---|---|
| `Ctrl+O` | 打开文件 | `Ctrl+W` | 关闭当前书，回书架 |
| `Ctrl+U` | 打开网页小说 | `Ctrl+G` | 手机阅读（局域网地址） |
| `Ctrl+F` | 搜索 | `F3` | 下一个命中 |
| `Ctrl+B` | 书签列表 | `Ctrl+D` | 添加书签 |
| `Ctrl+T` | 收起 / 展开目录 | `F2` | 日间 / 夜间切换 |
| `F6` | 自动下滑开关 | `F7` / `F8` | 减速 / 加速 |
| `Ctrl+=` / `Ctrl+-` | 字号大小 | `Ctrl+0` | 重置字号 |
| `空格` / `PgDn` | 下一页 | `PgUp` | 上一页 |
| `↑` `↓` `←` `→` | 逐行 / 逐字滚动 | `Home` / `End` | 全书开头 / 结尾 |
| `Alt+←` / `Alt+→` | 上一章 / 下一章 | `F5` | 重新加载当前书 |
| `Ctrl+C` / `Ctrl+A` | 复制选中 / 全选 | `F1` | 快捷键说明 |
| `F9` | PDF：文字重排 / 原始版式切换 | `Ctrl+Shift+G` | 跳到手机上读到的位置 |

鼠标：滚轮滚动，拖滚动条，点目录跳章，双击页边距快速翻页，拖动即可选中文字（松开自动复制）。

---

## 分章规则

一行的整理后文本满足下面全部条件时，认作章节标题：

1. 长度不超过 60 个字，且**不含句号**（避免把正文里的「第三章」句子当标题）；
2. 命中以下任一模式：
   - `第` + 数字/中文数字 + `章` / `回` / `节` / `卷` / `集` / `篇` / `幕` / `话`
   - `序章`、`序`、`楔子`、`引子`、`前言`、`后记`、`尾声`、`终章`、`番外`、`结剧`
   - `Chapter N` / `CHAPTER N`（英文写法）
3. 排除误判：`第三回合`、`第节课`、`五月天` 这类后面紧跟别的字的写法不算。

PDF / EPUB / DOCX / HTML / Markdown 会**先抽出纯文本**，再走同一套分章逻辑，
所以在这些格式里看到的目录和 TXT 是一致的行为。

---

## 书签 / 字体 / 配色

- **书签**：`Ctrl+D` 在当前位置插入书签（落在段落开头），正文左侧出现一条米色竖条；
  `Ctrl+B` 列出所有书签，点一条跳过去，`删除此段书签` 删掉当前位置的书签。
- **字体**：`阅读 → 外观 → 选择字体` 打开系统字体对话框；`导入字体文件` 可以加载
  `.ttf/.otf/.ttc`（用 `AddFontResourceEx(..., FR_PRIVATE)` 私有加载，不会装进系统）。
- **配色**：`阅读 → 外观 → 配色方案` 有五种预设；`自定义配色` 逐项挑背景 / 正文 /
  标题 / 选中 / 悬停色。

---

## 网页小说

`Ctrl+U`（或 文件 → 打开网页小说）粘上**目录页**网址：

1. 抓取目录页，把所有 `<a>` 链接里"文字像章节名"的挑出来（长度 ≤ 48 字、命中分章规则、同站点）；
2. 按目录顺序逐章抓取，剥掉标签、丢掉导航行、去掉重复标题，拼成一本完整 TXT；
3. 存到 `%LOCALAPPDATA%\NovelReader\web\书名.txt`，然后像本地书一样读、搜、记进度。

如果给了只有一章的单页网址，就把这一页当全书。抓取时可随时按 `ESC` 取消。

---

## 手机阅读

`Ctrl+G` 或 帮助 → 手机阅读，程序会启动一个局域网 HTTP 服务（默认端口 8231），
弹出形如 `http://192.168.1.5:8231/` 的地址。手机连**同一个 WiFi**，浏览器打开这个
地址就能读：书架、目录、上一页/下一页，都是为手机排版的大字号深色页面。

手机上的阅读进度会写进电脑上的同一份 `NovelReader.ini`，所以两边天然同步；
回到电脑按 `Ctrl+Shift+G` 直接跳到手机读到的地方。

> 这份实现没有 APK：本机没有 Java / Android SDK，也没有签名证书。局域网网页方案
> 不需要安装、不受应用商店限制，是目前最省事的做法。

---

## 自己编译

只依赖 **LLVM-MinGW**（或任何自带 `windres` 的 MinGW-w64），不需要 Visual Studio：

```bat
cd 源码
build.bat
```

产物 `build\NovelReader.exe`。实际用到的编译参数：

```bat
windres -I src -I res -O coff res\app.rc -o build\app.res
g++ -std=gnu++17 -O2 -DUNICODE -D_UNICODE -D_WIN32_WINNT=0x0601 -municode -mwindows ^
    -ffunction-sections -fdata-sections -Wl,--gc-sections -Wl,-s ^
    -static -static-libgcc -static-libstdc++ ^
    src\*.cpp build\app.res -o build\NovelReader.exe ^
    -lcomctl32 -lcomdlg32 -lshell32 -lole32 -lgdi32 -luser32 -ladvapi32 -lwininet -lws2_32 -liphlpapi
```

`-static -s` 让 exe 不依赖任何 DLL 且去符号；`--gc-sections` 只能救回几 KB，
真正省体积的是"不用 C++ 异常、不用 STL 流、不用第三方库"。

---

## 仓库结构

```
NovelReader/
├─ src/                  源码（每个文件头部都有中文说明）
│  ├─ ids.h              菜单项 / 控件 ID 号段约定
│  ├─ common.h           数据结构、全局状态、四个模块对外接口
│  ├─ book.cpp           文件解码、编码识别、章节划分、全文搜索
│  ├─ format.cpp/.h      EPUB / DOCX / HTML / Markdown 抽正文
│  ├─ zip.cpp/.h         自写 zip 容器 + DEFLATE (RFC1951) 解压
│  ├─ pdf.cpp/.h         自写 PDF 解析：对象扫描、FlateDecode、ToUnicode、文本重建
│  ├─ pdfrender.cpp/.h   用系统 Windows.Data.Pdf 把整页渲染成位图（原始版式）
│  ├─ web.cpp/.h         网页小说抓取（WinInet + 目录识别 + 逐章下载）
│  ├─ server.cpp/.h      手机阅读：局域网 HTTP 服务
│  ├─ ini.cpp            ini 读写、设置与阅读进度、书架数据
│  ├─ view.cpp           阅读视图：排版、换行、滚动、选择、绘制
│  ├─ ui.cpp             主窗口、菜单、侧栏、搜索条、各种对话框
│  └─ trace.cpp          调试日志（仅在 -DNR_TRACE 编译时启用）
├─ res/                  图标 + 资源脚本 + manifest
├─ screenshots/          README 用的截图
├─ build.bat             一键编译（GBK 编码，cmd 直接跑）
└─ dist/                 使用说明、下发用的示例小说
```

---

## 实现要点（几个自己比较在意的决定）

- **为什么自绘正文而不用 RichEdit**：十几 MB 的小说塞进 RichEdit 要几百 MB 内存、
  打开要好几秒，而且换字体、行距、主题都要跟控件的脾气打；自绘只画当前屏幕能看到的
  行，段落行数懒计算，滚动永远跟手。
- **为什么自己写 zip 和 DEFLATE**：为了 EPUB / DOCX 引 zlib 要多 100 KB 以上；
  手写一份 RFC1951 解压器不到 30 KB，还省掉了 DLL 依赖。
- **PDF 为什么两种看法都留着**：渲染成图片版式最忠实，但那一屏的字搜不了、换不了字体、
  夜间模式也白搭；抽文字重排能搜索能排版，但分栏 / 表格 / 图片会丢。所以两条路都做，
  `F9` 让用户按场景自己选，两种模式共用同一份文字，阅读位置、章节、书签都连续。
- **为什么手写 WinRT 接口声明**：MinGW 没有 C++/WinRT 也没有 `windows.data.pdf.h`，
  于是按 Windows 元数据把 `IPdfDocument`、`IPdfPage`、`IPdfPageRenderOptions` 的虚表
  逐个手写出来，再用 `RoGetActivationFactory` 拿工厂 —— 只花了几百行，省掉一个 MSVC
  工具链，也不用多带任何 DLL。
- **为什么限制单实例**：两个实例同时写同一份 ini 会互相覆盖进度，用户会看到
  "读到一半的进度莫名其妙回退"。现在第二个实例把自己的文件参数转发给第一个就退出。

---

## 性能参考（本机 Windows 10 22H2 / i5）

| 场景 | 结果 |
|---|---|
| 启动到出现窗口 | 约 0.12 秒 |
| 14.7 MB / 8794 章 TXT | 0.8 秒载入，滚动流畅 |
| 全文搜索 7 万字 | 即时（`< 20 ms`） |
| 全文搜索 14.7 MB | 约 120 ms（一次性扫完，结果可到 4000 条） |
| 内存占用（14.7 MB 的书） | 约 60 MB（正文 + 段落索引 + 章节表） |
| 程序体积 | 530 KB（静态链接，无 DLL 依赖） |

---

## 常见问题

**打开是乱码？** 自动识别 UTF-8 / GBK / GB18030 / UTF-16；若仍乱码，请确认文件不是
压缩包或二进制文件。可在「帮助 → 关于」看到当前文件的识别编码。

**目录里没有章节？** 说明正文里的章节标题不合分章规则（比如没有"第X章"字样，或者
标题里带句号）。这种情况会退化成按段落翻页，搜索和记忆位置照常可用。

**进度丢了？** 进度写在 exe 同目录的 `NovelReader.ini`。如果程序放在只读目录，
会自动改写到 `%APPDATA%\NovelReader\NovelReader.ini`。

**PDF 打开是空白？** 说明这份 PDF 没有文字层（扫描件），或者用了对象流压缩 / 加密 ——
这种情况下文字模式读不出内容，但按 `F9` 切到「原始版式」照样能一页页看（版式按原样渲染）。

**PDF 原始版式字太小？** 把窗口拉大即可等比放大；想换字体、调配色、搜索，`F9` 切回文字重排。

**网页抓取失败？** 检查网址是否为目录页、站点是否需要登录（本程序不带 cookie），
以及本机代理设置；抓取时会先走系统代理，失败再直连重试。

---

## 反馈

欢迎提 [Issue](https://github.com/chenzy0623/NovelReader/issues)。如果遇到打不开的书，
把文件（或前几 KB）附上最有助于定位。

## 许可

MIT License，见 [LICENSE](LICENSE)。

## 作者

| 顺序 | 作者 | 分工 |
| --- | --- | --- |
| 第一作者 | [chenzy0623](https://github.com/chenzy0623) | 需求、设计取舍、验收与发布 |
| 第二作者 | xuanmo 的大肥鱼 | 编码、调试、文档与截图 |

Copyright (c) 2025 chenzy0623, xuanmo 的大肥鱼

