import os
import random

HERE = os.path.dirname(os.path.abspath(__file__))
TESTDIR = os.path.join(os.path.dirname(HERE), "test")
random.seed(20240607)

titles = ["序章 风起青萍", "楔子 长夜", "第一章 少年出山", "第二章 荒古禁地", "第三章 药老",
          "第四章 三年之约", "第五章 初入迦南", "第六章 炼药师大会", "第七章 云岚宗",
          "第八章 魂殿初现", "第九章 天焚炼气塔", "第十章 陨落心炎", "第十一章 黑角域",
          "第十二章 中州风云", "第十三章 星陨阁", "第十四章 丹会", "第十五章 斗圣遗迹",
          "第十六章 古族", "第十七章 净莲妖火", "番外 药老的往事", "结局 大结局"]

body_pool = [
    "夜色如墨，山风穿过松林，带来一阵清冷的松脂气味。",
    "他抬头望向远处那座被云雾笼罩的山峰，眼中闪过一抹不易察觉的坚定。",
    "“三年之后，我会回来的。”少年低声说道，声音不大，却像钉子一样钉进了风里。",
    "药老的身影在白光中渐渐凝实，苍老的脸上带着几分笑意：“小子，你这一路走来，倒是比老夫想的更狠。”",
    "萧炎握紧了手中的玄重尺，尺身古朴，其上隐约流转着淡淡的火芒。",
    "斗气在经脉中奔涌，如同一条灼热的河流，冲刷着每一处阻塞。",
    "迦南学院的大门前，人潮涌动，来自各方的少年天才在此汇聚。",
    "“斗之气，三段！”测验魔石碑上，五个大字刺眼地亮起。",
    "异火的气息自深处涌出，整个塔身都开始微微震颤，仿佛有一头沉睡的巨兽正在苏醒。",
    "云岚宗的山峰之上，云韵一袭白衣，静静立于崖边，风把她的裙角吹得猎猎作响。",
    "拍卖场里的灯光明亮而柔和，木质的栏杆上雕着繁复的花纹。",
    "那枚丹药静静地躺在玉瓶之中，通体浑圆，表面萦绕着一缕略带紫意的丹香。",
    "空间在那一刻仿佛凝固了，只有那道刀光还在向前，一寸一寸地推进。",
    "黑角域的规矩很简单：谁的拳头硬，谁就是规矩。",
    "魂殿的黑袍人立于半空，声音嘶哑：“把那东西交出来，本座可以让你死得痛快些。”",
    "中州的天空常年灰蒙，辽阔的平原上，一座座城池如同棋子散布。",
    "丹会之上，数百口丹炉同时燃起，火光把整片广场映得通红。",
    "古族的古界深处，古老的石碑上刻满了名字，每一个都曾是一个时代的巅峰。",
    "净莲妖火静静地漂浮在那里，如同一朵开在虚空中的白色莲花，美得让人心悸。",
    "他笑了，笑声很轻，却带着一种终于走到尽头的释然。",
]

names = ["萧炎", "药老", "云韵", "薰儿", "美杜莎", "小医仙", "风尊者", "纳兰嫣然"]

def paragraph():
    n = random.randint(1, 3)
    return "".join(random.choice(body_pool) for _ in range(n))

def chapter(idx):
    if idx == 0:
        title = titles[0]
    elif idx == 1:
        title = titles[1]
    else:
        t = titles[(idx + 1) % len(titles)]
        name = t.split(" ", 1)[1] if " " in t else t
        title = "第%d章 %s" % (idx - 1, name)
    lines = [title, ""]
    for _ in range(random.randint(6, 12)):
        lines.append(paragraph())
        lines.append("")
    return "\n".join(lines) + "\n\n"

def make_book(path, nch, encoding):
    parts = ["斗破苍穹（测试样例）\n\n作者：天蚕土豆\n\n本书为源码测试用的自动生成文本，非真实章节内容。\n\n\n"]
    for i in range(nch):
        parts.append(chapter(i))
    text = "".join(parts)
    with open(path, "w", encoding=encoding, errors="replace", newline="\r\n") as f:
        f.write(text)
    return len(text)

os.makedirs(TESTDIR, exist_ok=True)
a = make_book(os.path.join(TESTDIR, "斗破苍穹_utf8.txt"), 120, "utf-8")
b = make_book(os.path.join(TESTDIR, "测试小说_gbk.txt"), 45, "gbk")

# 章节识别专项测试：各种写法的标题 + 一些不该被当成章节的行
probe = [
    "第1章 开始",
    "第一章 开始",
    "第001章 带前导零",
    "第 12 章 带空格",
    "第100章 大数字",
    "第一百二十三章 中文数字",
    "第3回 回目",
    "第5节 小节",
    "第三回合的时间到了。",
    "第2章他走了",                     # 不带空格
    "Chapter 5 The Beginning",
    "CHAPTER 12",
    "序章 风起",
    "楔子",
    "番外 药老的往事",
    "终章 落幕",
    "第7卷 卷名",
    "第十二篇",
    "这是正文，第一段，不该被识别。",
    "他说：第一章我没看。",             # 含句号，不该识别
    "第9章",                            # 纯标题
]
with open(os.path.join(TESTDIR, "章节识别测试.txt"), "w", encoding="utf-8", newline="\r\n") as f:
    for i, line in enumerate(probe):
        f.write(line + "\n")
        f.write("这一段是正文占位内容，用来把段落分开。" + str(i) + "\n\n")

# 大文件：约 5MB，用于测试加载与滚动
big = ["大文件测试\n\n"]
i = 0
while sum(len(x) for x in big) < 5 * 1024 * 1024:
    big.append(chapter(i))
    i += 1
with open(os.path.join(TESTDIR, "大文件_utf8.txt"), "w", encoding="utf-8", newline="\r\n") as f:
    f.write("".join(big))

for n in ("斗破苍穹_utf8.txt", "测试小说_gbk.txt", "大文件_utf8.txt"):
    p = os.path.join(TESTDIR, n)
    print(n, os.path.getsize(p), "bytes")
print("chapters in big:", i)
