# 小说阅读器 NovelReader

一个**单文件、免安装、零依赖**的 Windows 小说阅读器。用 C++17 + Win32 从零写成，
**1013 KB** 一个 exe，双击就能用，不写注册表、不装运行库、不需要联网。

支持 **TXT / EPUB / PDF / DOCX / HTML / Markdown**，自动识别章节，全文搜索，
记忆阅读位置，自动下滑，书签，换字体，自定义配色，还能直接把网页小说抓成本地书。

![版本](https://img.shields.io/badge/version-1.7.0-blue)
![体积](https://img.shields.io/badge/size-1013%20KB-brightgreen)
![平台](https://img.shields.io/badge/platform-Windows%2010%20%2F%2011-lightgrey)
![依赖](https://img.shields.io/badge/dependencies-none-success)
![许可](https://img.shields.io/badge/license-MIT-orange)

**作者**：[chenzy0623](https://github.com/chenzy0623)（第一作者） · xuanmo 的大肥鱼（第二作者）

---

## 下载与运行

到 [Releases](https://github.com/chenzy0623/NovelReader/releases) 下载最新版：

- **[NovelReader-1.7.0-win64.zip](https://github.com/chenzy0623/NovelReader/releases/download/v1.7.0/NovelReader-1.7.0-win64.zip)**（推荐，含使用说明和示例小说）
- [单个 NovelReader.exe](https://github.com/chenzy0623/NovelReader/releases/download/v1.7.0/NovelReader.exe)（只要程序本体）
- **[NovelReader-1.7.0.apk](https://github.com/chenzy0623/NovelReader/releases/download/v1.7.0/NovelReader-1.7.0.apk)**（安卓端，69 KB，Android 5.0+ 直接装）

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
| 界面 | 顶栏胶囊按钮 + 书架卡片 + 自绘状态栏进度线，不是系统控件拼的 |
| 升级不丢书 | 新版本放在新目录里也能自动找回旧书架与阅读进度 |
| 每本书独立排版 | 字号、行距、页边距、首行缩进、分页、双栏、强制字体都按书名分别记，读网文和读古籍互不干扰 |
| 分页 / 双栏 | 按「本章第几页 / 共几页」翻页，可切双栏横排（自动开分页）；`Ctrl+Shift+P` / `Ctrl+Shift+C` |
| 高亮与笔记 | 4 种颜色标重点（`Ctrl+H`），每处可写笔记、列表里带 ✎、一键导出 Markdown |
| 快捷键自定义 | `Ctrl+F12` 里逐条改键，按一下新组合就生效，可恢复默认 |
| 老板键 / 隐私模式 | `Ctrl+Alt+H` 一键藏窗口（全局热键，切走也能召回）；隐私模式把书名换成「文档」 |
| 手机同步 | 手机装 APK，和电脑在同一个 WiFi 里各自打开「同步」，两边读到哪互相比对，进度差得多会先问你要不要跳 |
| 书源搜索 | `Ctrl+S` 按书源搜书名/作者，选中就下载整本、分好章直接读；书源可整套替换（见下） |
| 手机阅读 | 局域网 HTTP 服务，手机浏览器直接读（菜单位置：帮助 → 手机阅读） |
| 大文件 | 14.7 MB / 8794 章实测 0.8 秒载入，只渲染可视区域，滚动不掉帧 |
| 目录收起 | 点侧栏底部「‹ 收起目录」或 `Ctrl+T`，正文占满整屏 |
| 单实例 | 重复双击不会开出第二个窗口互相覆盖进度，而是把文件转发给已开着的那个 |

---

## 界面截图

主界面（TXT 大文件，左侧目录 + 正文）：

![首次打开](https://github.com/chenzy0623/NovelReader/raw/main/screenshots/09-welcome.png)

*第一次打开（书架还是空的）：直接把书拖进来，或按 Ctrl+O。*

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

书源搜索（Ctrl+S，按当前书源搜书，搜到就下载整本）：

![书源搜索](https://github.com/chenzy0623/NovelReader/raw/main/screenshots/15-source-search.png)

书源管理（六项规则，能改、能加、能删，内置的那个也能整套换成你自己的）：

![书源管理](https://github.com/chenzy0623/NovelReader/raw/main/screenshots/17-source-manager.png)

下载完直接读（目录已经分好章）：

![书源下载](https://github.com/chenzy0623/NovelReader/raw/main/screenshots/16-source-book.png)

---

## 界面（1.5.0 重新画过）

1.5.0 把界面从"系统控件拼起来"改成自绘的纸感风格：

- **顶栏**：左上三条横线是「菜单」（`Ctrl+F10` 也能弹），右边一排胶囊按钮
  —— 目录 / 打开 / 搜索 / 自动 / 书源 / 主题，按下去会高亮；书名和当前章名在左侧，
  不再单独占一条标题栏。
- **书架**：卡片网格，每张卡片有书脊色条、书名、右上角进度、当前章节名、底部细进度条；
  鼠标移上去会亮一下，单击直接续读。
- **目录 / 搜索结果 / 书签**：同一个列表控件自绘，选中行是圆角色块 + 左侧强调色竖条；
  收起目录后只剩一条 18 px 宽的窄条，箭头是矢量画的（不依赖字体里有没有那个字形）。
- **状态栏**：书名 / 章节 / 进度三段，底部一条通栏进度线；自动下滑时右侧多一个 `▸自动 4`。
- **搜索条**：输入框和三个按钮都跟着配色走，没有系统灰和立体边框。
- 菜单栏被拿掉了（系统灰条在夜间配色下是全场唯一亮色），改成「菜单」按钮 + 右键弹出的弹出式菜单。

---

## 1.7.0 新功能：安卓端 + 局域网阅读同步

电脑上读到第三章，出门用手机接着读；回家打开电脑，它问你「手机读到 30% 了，要跳过去吗」。

### 怎么用

1. 电脑端：菜单（左上三条横线）→ 帮助 → **开启手机同步（局域网）**，状态栏会显示「阅读同步已开启，正在局域网里找手机…」。
2. 手机端：装 `NovelReader-1.7.0.apk`，打开后点右上角 **同步** 开关。
3. 两端在同一个 WiFi 下，几秒内互相就能看到（UDP 广播发现，不用填 IP、不用连同一个账号）。
4. 电脑上打开一本书 → 手机上点开同一本 → 两边各自读；换到另一台设备打开同一本书时，进度差超过约半屏，会弹一句问话：
   - 点「同步」：跳过去，按对方的位置继续；
   - 点「不用」：留在本机，并把本机进度推给对方（下次对方打开就会问它）。

对话示例（纸感消息框，跟着主题走）：

![手机同步问句](screenshots/22-sync-dialog.png)

### 三条设计取舍

- **没有服务器，也没有账号**。两端各自监听一个 TCP 端口（8233，被占用就往上找）和一个 UDP 端口（8234，只做发现），谁都不当"固定服务端"——这样电脑关机、手机换 WiFi 都不会让另一边变成孤儿。
- **靠正文指纹认书，不靠文件名**。`FNV-1a 64` 哈希「书名 + 正文长度 + 前 4096 字」，两端算出同样的 16 位 key 才认为是同一本书。所以手机上那本叫「斗破苍穹.txt」、电脑上叫「斗破苍穹_utf8.txt」，照样能对上；而两本不同的书恰好同名也不会串。
- **同步是"用户自己开启"的**。默认关闭，不开就不监听任何端口；开着时也只是局域网内的点对点直连，没有云端中转。

### 安卓端

- 纯 Java + 原生控件（无 AndroidX、无 Gradle），APK 69 KB；用 `aapt2` + `javac` + `d8` + `zipalign` + `apksigner` 手工构建，把构建脚本和源码一起放进了仓库的 `android/`。
- 手机端能读的书来自电脑：电脑这边**读过**的书（书架上有记录的）每一本都带指纹，手机点开时按指纹把正文要过去，缓存在自己 `filesDir/books/` 里，之后离线也能看。
- 手机端也会把自己的阅读位置报给电脑（每次翻页上报页首字符下标，与电脑端同一套坐标）。

> 没有模拟器也没有真机，所以安卓端只做到「编译通过 + 签名校验 + 协议用 JVM 直接跑通（`/peer/ping`、`/peer/books`、`/peer/book`、`/peer/progress` 全部与电脑端互通）」；界面截图欠奉，问题请在 Issues 里反馈。

---

## 1.6.0 新功能：按书排版 / 高亮笔记 / 改键 / 老板键

### 每本书独立的排版

同一份程序里，读网文可能想要大字号、宽行距、双栏；读古籍或 PDF 抽出来的文本又想要小字号、单栏、不缩进。1.5.0 之前这些是全局设置，来回切书就得来回改。

1.6.0 把排版记到**每本书自己的 ini 段**里：打开哪本，就套哪本上次的排版。

| 设置 | 键名 | 取值 |
| --- | --- | --- |
| 字号 | `lay_fontPt` | 9 ~ 72 磅 |
| 行距 | `lay_spacing` | 0 紧凑 / 1 标准 / 2 宽松 |
| 页边距 | `lay_margin` | 0 窄 / 1 标准 / 2 宽 |
| 首行缩进 | `lay_indent` | 0 关 / 1 开（两字） |
| 分页模式 | `lay_paged` | 0 连续滚动 / 1 按页翻 |
| 分栏 | `lay_cols` | 1 单栏 / 2 双栏 |
| 强制字体 | `lay_force` | 0 用导入的字体优先 / 1 强制用系统字体 |
| 字体名 | `lay_face` | 任意已装或已导入的字体 |

新书第一次打开时继承「设置」里的默认字号/行距/字体（也就是你上次调整的那套），之后各记各的。

![每本书独立排版](screenshots/18-layout-cols.png)

> 双栏会自动开分页——连续滚动时两栏没法同步位置，硬做只会更别扭。

### 高亮与笔记

- 选中一段文字按 `Ctrl+H`（或菜单「阅读 → 高亮与笔记」）标高亮，颜色四种：黄 / 绿 / 蓝 / 粉；没选中时整段标。
- 高亮块用「主色 + 纸色」混色画，所以夜间、护眼绿、自定义配色下都不会刺眼。
- 同一处再按 `Ctrl+Shift+N` 写笔记；列表里带 ✎ 标记。
- `Ctrl+L` 打开高亮 / 笔记列表，点一条就跳过去。
- 「导出笔记」生成 `<书名>-笔记.md`，按章节分组，可以直接贴进 Obsidian。

![高亮与笔记列表](screenshots/19-highlight.png)

### 快捷键随便改

`Ctrl+F12` 打开快捷键设置：选中一条，直接按新的组合（比如 `Ctrl+Shift+1`、`Alt+K`、`F12`），立刻生效并存盘。`Esc` / `Delete` 清空该条（清掉就等于不用快捷键）。要字母键的话必须带 `Ctrl` 或 `Alt`，防止打字时误触。

25 条命令都能改，老板键也在里面（默认 `Ctrl+Alt+H`）。

### 老板键与隐私模式

- **老板键**：`Ctrl+Alt+H` 立刻把窗口藏起来。这是系统级热键（`RegisterHotKey`），窗口藏起来、焦点在别的程序上也能按回来。注册失败（被别的软件占了）会在状态栏提示。
- **隐私模式**：`Ctrl+Shift+H`，窗口标题里的书名换成「文档」，任务栏和 Alt+Tab 里也不露出你在读什么。开关状态记在 ini 里。
## 升级 / 换目录之后，书架不会空

旧版把书架记在 exe 同目录的 `NovelReader.ini` 里，于是"把新版解压到新文件夹"看起来就像丢书
（其实书和进度都还在，只是程序没去那个目录看）。1.5.0 加了两层兜底：

1. **启动时自动找旧记录**：exe 同目录的 ini 里没有阅读记录时，会往上看 1~2 级目录、
   看 `程序` / `NovelReader` 这类子目录，再看 `%APPDATA%`、`%LOCALAPPDATA%` 和桌面上的常见位置，
   找到第一份带阅读记录的 ini 就直接改用那一份（不复制、不合并 —— 合并会弄乱你手改过的书源规则）。
2. **书架能自愈**：即使 `[recent]` 段被写坏或清空，只要 ini 里还留着每本书的进度段，
   书架就会按里面的路径把书补回来，并按最后阅读时间从新到旧排好。

> 真想从头开始：删掉 ini 即可。想固定用某一份：把它放到 exe 旁边。

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
| `Ctrl+S` | 书源搜索（可选书源） | `Ctrl+U` | 打开网页小说（任意网址） |
| `Ctrl+F` | 搜索 | `F3` | 下一个命中 |
| `Ctrl+B` | 书签列表 | `Ctrl+D` | 添加书签 |
| `Ctrl+T` | 收起 / 展开目录 | `F2` | 日间 / 夜间切换 |
| `F6` | 自动下滑开关 | `F7` / `F8` | 减速 / 加速 |
| `Ctrl+=` / `Ctrl+-` | 字号大小 | `Ctrl+0` | 重置字号 |
| `空格` / `PgDn` | 下一页 | `PgUp` | 上一页 |
| `↑` `↓` `←` `→` | 逐行 / 逐字滚动 | `Home` / `End` | 全书开头 / 结尾 |
| `Alt+←` / `Alt+→` | 上一章 / 下一章 | `F5` | 重新加载当前书 |
| `Ctrl+C` / `Ctrl+A` | 复制选中 / 全选 | `F1` | 快捷键说明 |
| `F9` | PDF：文字重排 / 原始版式切换 | `Ctrl+G` | 手机阅读（局域网地址） |
| `Ctrl+Shift+G` | 跳到手机上读到的位置 | `Ctrl+F10` | 弹出主菜单（界面上没有菜单栏） |
| `Ctrl+H` | 高亮当前段（沿用上次颜色） | `Ctrl+Shift+N` | 给高亮写笔记 |
| `Ctrl+L` | 高亮 / 笔记列表 | `Ctrl+Shift+X` | 删除当前段的高亮 |
| `Ctrl+Shift+P` | 分页模式开关 | `Ctrl+Shift+C` | 双栏开关 |
| `Ctrl+Shift+I` | 首行缩进开关 | `Ctrl+Shift+H` | 隐私模式（书名显示为「文档」） |
| `Ctrl+Alt+H` | 老板键：隐藏 / 唤回窗口 | `Ctrl+F12` | 快捷键设置 |

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

## 书源搜索 / 自定义书源

`Ctrl+S`（或 文件 → 书源搜索）输入书名或作者，按当前书源搜书。程序里带了**一个示例书源**，
它只是一组规则，你可以整套换掉，也可以再加新站。

1. 结果列表每条显示 `书名 · 简介`，双击或选中后点「下载并打开」；
2. 程序读详情页的章节目录，按顺序逐章下载，拼成一本本地 TXT，存到
   `%LOCALAPPDATA%\NovelReader\web\书名.txt`，然后直接打开阅读；
3. 抓取时进度显示在主窗口状态栏，`ESC` 可中途取消 —— 取消后会问要不要把已抓到的部分存下来。

### 书源怎么配（文件 → 书源管理）

一个书源就是**六项**，全存在 `NovelReader.ini` 的 `[sources]` 段里（最多 16 个源，`cur` 是当前选中）：

| 键 | 含义 |
|---|---|
| `s0_name` | 名字，随便写，只用来在下拉框里区分 |
| `s0_search` | 搜索网址，关键词的位置写 `{kw}`；不写 `{kw}` 就把关键词拼在末尾 |
| `s0_list` | 结果页正则：第 1 组是书籍网址，第 2 组是书名，第 3 组可选（简介） |
| `s0_tocstart` | 目录容器的起点文字（普通子串，不是正则），页面先切到这里再找目录，免得把"推荐位"收进目录 |
| `s0_toc` | 目录正则：第 1 组是章节网址，第 2 组是章节名 |
| `s0_content` | 正文正则：第 1 组是正文那段 HTML |

几条容错设计：

- **留空不报错**：`s0_list` 空就走通用链接收集；`s0_toc` 空就走通用章节过滤；`s0_content` 空就把整页转文字；
- **正则写错有提示**：报「书源里的正则写得不对（括号不配对或转义有误）」；
- **目录宁多勿少**：正则抓到的名字如果全都不像章节名，就全部收下，避免整本变空；
- **示例书源删不掉**：它是 0 号，想换成自己的站，直接改它的六项、点「保存」即可，也能随时「恢复内置规则」。

> 为什么用正则而不是 CSS 选择器：程序里没有 HTML DOM 解析器，标准库 `<regex>` 体积零成本，
> 一条正则够用，而且你可以直接从别处抄规则改。代价是 exe 从 582 KB 涨到 1013 KB（模板实例化）。

> 有些站挂在 Cloudflare 后面，会拿 TLS 指纹和请求头判断是不是机器人：WinInet 的请求一律 403，
> 所以代码里给这类站点留了 `curl.exe`（Win10 1803 起系统自带）兜底通道；每章之间还会歇 250 ms，
> 失败重试带退避。如果提示「站点触发人机验证」，通常是自己搜得太密被临时限流，等一两分钟再试。


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
│  ├─ web.cpp/.h         网页抓取 + 书源搜索（WinInet / curl 兜底 + 目录识别）
│  ├─ source.cpp/.h      书源模型：六项规则 + ini 存取 + 增删改
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
- **为什么书源要多一条 `curl.exe` 通道**：有些站挂在 Cloudflare 后面，会按 TLS 指纹
  判断客户端，WinInet / WinHTTP 的请求一律 403（`Just a moment...` 挑战页），而系统自带
  的 `curl.exe` 能过。所以 `WebFetchUrl` 的尝试顺序是「系统代理 → 直连 → `curl.exe`」，
  一旦某站被判定只能走 curl 就记住它，后续每章直接走那条路，省掉注定失败的两次往返。
- **为什么书源是可配置规则而不是写死某个站**：写死意味着站一改版程序就废、用户也没法换源。
  现在书源只是一组"搜索网址 + 三条正则 + 目录起点"，存在 ini 里，界面能改、能加、能删；
  规则留空就退化成通用抓取，所以"没配好"也不会崩，只是抓得粗一点。
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
| 内存占用（14.7 MB 的书） | 约 15 MB 私有内存（正文一次性解码进内存，段落索引只多几 MB） |
| 程序体积 | 1013 KB（静态链接，无 DLL 依赖） |

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

**更新到新版后书架空了？** 1.5.0 起会自动找回旧记录（见上面「升级 / 换目录之后，书架不会空」）。
如果还是空，说明旧 ini 已经不在这台机器上了 —— 直接重新打开一次书，进度会从新记录开始。

**内存占用多大？** 空窗口约 12 MB 工作集；打开 14.7 MB 的小说约 15 MB 私有内存（PDF 的
「原始版式」因为要渲染整页位图会到 28 MB 左右，这属 Windows 自带 PDF 引擎的开销）。

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

