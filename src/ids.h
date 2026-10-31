/* ids.h - 命令 ID、控件 ID 与资源 ID 的统一定义
 *
 * 这个头文件被两处共用：
 *   1) C++ 源码：决定 WM_COMMAND 里 wParam 的低字（命令/控件 ID）
 *   2) 资源脚本 res\app.rc：引用图标等资源
 * 因为 windres 只是 C 预处理器，不认识 enum / constexpr，所以这里全部用
 * #define 纯数字宏，不要改成别的写法。
 *
 * ID 号段约定（数值区间本身就是分类，出问题时看数字就能猜到是谁）：
 *   1000~1099  文件类命令（打开 / 重新载入 / 退出 / 最近打开子菜单）
 *   1100~1199  最近打开列表的动态 ID 区：IDM_RECENT_BASE + 序号(0..9)
 *   1200~1216  阅读类命令（翻章 / 字号 / 主题 / 目录 / 行距 / 复制 / 全选）
 *   1220~1229  搜索类命令
 *   1230~1239  帮助类命令
 *   1240~1242  自动下滑
 *   2000~2099  子控件 ID（列表、编辑框、按钮、视图、状态栏）
 *   3000~3099  资源 ID（图标、加速键表）
 *
 * 一条经验：菜单项、按钮这类"命令"用 IDM_ 前缀并发到主窗口；
 * "控件"用 IDC_ 前缀，既用于创建控件，也用于 WM_COMMAND 通知（如按钮点击）。
 */
#ifndef NR_IDS_H
#define NR_IDS_H

/* ---- 文件菜单 ---- */
#define IDM_OPEN        1001    /* 打开小说文件（Ctrl+O），支持 txt/epub/docx/html/md 等 */
#define IDM_RELOAD      1002    /* 重新载入当前文件（F5），文件被改动过时用 */
#define IDM_EXIT        1003    /* 退出：会先保存阅读进度和设置 */
#define IDM_CLOSEFILE   1004    /* 关闭当前书，回到首页书架（Ctrl+W） */
#define IDM_RECENT_BASE 1100    /* 最近打开列表的起始 ID，实际用 1100+i */
#define IDM_RECENT_MAX  10      /* 最近打开最多记 10 条 */

/* ---- 阅读菜单 ---- */
#define IDM_PREV_CH     1200    /* 上一章（←） */
#define IDM_NEXT_CH     1201    /* 下一章（→） */
#define IDM_FONT_UP     1210    /* 字号增大（Ctrl+=），上限 48 磅 */
#define IDM_FONT_DOWN   1211    /* 字号减小（Ctrl+-），下限 10 磅 */
#define IDM_THEME       1212    /* 日间 / 夜间主题切换（F2） */
#define IDM_TOC         1213    /* 显示 / 隐藏左侧目录（Ctrl+T） */
#define IDM_LINE_SPACE  1214    /* 行距三档循环切换（紧凑 / 标准 / 宽松） */
#define IDM_COPY        1215    /* 复制选中的文字（Ctrl+C） */
#define IDM_SELALL      1216    /* 全选正文（Ctrl+A） */

/* ---- 搜索菜单 ---- */
#define IDM_FIND        1220    /* 打开搜索条（Ctrl+F） */
#define IDM_FIND_NEXT   1221    /* 跳到下一个搜索结果（F3） */
#define IDM_FIND_PREV   1222    /* 上一个结果（预留，当前未挂菜单） */
#define IDM_FIND_CLOSE  1223    /* 关闭搜索条 / 搜索结果面板（Esc） */

/* ---- 帮助菜单 ---- */
#define IDM_KEYS        1230    /* 快捷键说明对话框 */
#define IDM_ABOUT       1231    /* 关于对话框（含 ini 路径） */

/* ---- 自动下滑（速度可调） ---- */
#define IDM_AUTO_TOGGLE 1240    /* 开 / 关自动下滑（F6） */
#define IDM_AUTO_FAST   1241    /* 加速（F8），1~10 级，级数越大滚动间隔越短 */
#define IDM_AUTO_SLOW   1242    /* 减速（F7） */

/* ---- 外观：字体 / 配色（1250~1269） ---- */
#define IDM_PICK_FONT   1250    /* 选正文字体（走系统字体对话框，可挑字体名和字号） */
#define IDM_IMPORT_FONT 1251    /* 导入 .ttf/.otf/..ttc 字体文件：私有加载，不装进系统，可反复导入 */
#define IDM_PICK_COLORS 1252    /* 自定义配色：背景 / 正文 / 标题 / 选中色各选一次 */
#define IDM_PRESET_BASE 1260    /* 预设配色方案起始 ID，实际用 1260+i（i=0..IDM_PRESET_MAX-1） */
#define IDM_PRESET_MAX  5       /* 预设方案个数：日间 / 夜间 / 护眼 / 羊皮纸 / 高对比暗 */

/* ---- 书签（1270~1279） ---- */
#define IDM_BM_ADD      1270    /* 给当前阅读位置加书签（Ctrl+D） */
#define IDM_BM_LIST     1271    /* 打开/切到书签列表（Ctrl+B） */
#define IDM_BM_DEL      1272    /* 删掉当前这一段上的书签 */

/* ---- 网页小说（1280~1299，预留） ---- */
#define IDM_OPEN_URL    1280    /* 从网址打开：抓网页正文（后续做成整本抓取） */
#define IDM_PHONE        1290   /* 手机阅读：开局域网服务并显示地址 */
#define IDM_PHONE_JUMP   1291   /* 跳到手机端最近读到的位置 */
#define IDM_PDF_MODE     1292   /* PDF 原始版式 / 文字重排 切换（F9，仅 PDF 有效） */
#define IDM_SOURCE       1293   /* 书源搜索：内置无忧书城，输入书名搜书并下载整本（Ctrl+S） */

/* ---- 子控件 ID ---- */
#define IDC_TOC         2001    /* 左侧章节目录 LISTBOX */
#define IDC_HITS        2002    /* 左侧搜索结果 LISTBOX（与目录互斥显示） */
#define IDC_FINDEDIT    2003    /* 搜索条里的输入框 EDIT */
#define IDC_BFIND       2004    /* 搜索条「搜索」按钮 */
#define IDC_BNEXT       2005    /* 搜索条「下一个」按钮 */
#define IDC_BCLOSE      2006    /* 搜索条「关闭」按钮 */

#define IDC_VIEW        2010    /* 自绘阅读视图子窗口（窗口类 NRView） */
#define IDC_STATUS      2011    /* 底部三段式状态栏 */
#define IDC_FOLD        2012    /* 侧栏折叠条：一条竖着的小按钮，点一下收起/展开左侧目录 */

/* ---- 资源 ID ---- */
#define IDI_APP         3001    /* 程序图标（res\app.ico，16~256 多尺寸） */
#define IDR_ACCEL       3010    /* 加速键表（当前用 CreateAcceleratorTableW 运行时建，此 ID 备用） */

#endif /* NR_IDS_H */
