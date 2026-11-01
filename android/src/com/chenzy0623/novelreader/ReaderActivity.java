package com.chenzy0623.novelreader;

import android.app.Activity;
import android.app.ProgressDialog;
import android.content.DialogInterface;
import android.content.Intent;
import android.graphics.Color;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.text.TextUtils;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewGroup;
import android.widget.LinearLayout;
import android.widget.TextView;
import android.widget.Toast;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.util.List;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

/**
 * 阅读页：把整本书按屏分页，左右三分之一区域翻页，翻页就把进度报给电脑。
 *
 * 分页为什么按"字符数"而不是真正的文字布局：
 *   手机上要精确排版得用 StaticLayout 逐行量，对 5 MB 的书很慢；
 *   按"一行能放几个汉字 × 一页几行"估一个字符数，再在最近的换行处断开，
 *   观感够用、翻页是常数时间。
 */
public class ReaderActivity extends Activity {

    private static final int C_BG   = 0xFFF7F3E9;
    private static final int C_FG   = 0xFF312D27;
    private static final int C_DIM  = 0xFF8C8578;
    private static final int C_TITLE= 0xFF8A5A2B;

    private final Handler ui = new Handler(Looper.getMainLooper());

    private String key = "";
    private String title = "";
    private String text = "";
    private int pageStart = 0;          /* 当前页第一个字符在全文里的下标 */
    private int charsPerPage = 800;
    private boolean ready = false;

    private TextView page;
    private TextView head;
    private TextView foot;

    @Override
    protected void onCreate(Bundle b) {
        super.onCreate(b);
        Intent it = getIntent();
        key = it.getStringExtra("key");
        title = it.getStringExtra("title");
        if (key == null) key = "";
        if (title == null) title = "";

        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setBackgroundColor(C_BG);

        head = new TextView(this);
        head.setTextColor(C_TITLE);
        head.setTextSize(13);
        head.setPadding(dp(16), dp(14), dp(16), dp(6));
        head.setSingleLine(true);
        head.setEllipsize(TextUtils.TruncateAt.END);
        root.addView(head);

        page = new TextView(this);
        page.setTextColor(C_FG);
        page.setTextSize(17);
        page.setLineSpacing(dp(8), 1f);
        page.setPadding(dp(20), dp(6), dp(20), dp(6));
        page.setIncludeFontPadding(false);
        root.addView(page, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f));

        foot = new TextView(this);
        foot.setTextColor(C_DIM);
        foot.setTextSize(12);
        foot.setGravity(Gravity.CENTER);
        foot.setPadding(0, dp(6), 0, dp(14));
        root.addView(foot);

        setContentView(root);

        /* 点击分区：左 1/3 上一页，右 1/3 下一页，中间什么都不做（防误触） */
        page.setOnTouchListener(new View.OnTouchListener() {
            @Override
            public boolean onTouch(View v, MotionEvent e) {
                if (e.getAction() != MotionEvent.ACTION_UP) return true;
                float w = v.getWidth();
                if (e.getX() < w / 3f) { flip(-1); return true; }
                if (e.getX() > w * 2f / 3f) { flip(1); return true; }
                return true;
            }
        });

        load();
    }

    /** 拿正文：优先本地缓存，没有就从电脑拉（拉完落盘，之后离线也能看）。 */
    private void load() {
        final File f = Store.bookFile(this, key);
        if (f.exists() && f.length() > 0) {
            text = readFile(f);
            ready = true;
            startReading();
            return;
        }

        final ProgressDialog pd = new ProgressDialog(this);
        pd.setMessage("正在从电脑取书…");
        pd.setCancelable(false);
        pd.show();

        new Thread(new Runnable() {
            @Override
            public void run() {
                String err = null;
                try {
                    Sync.get(ReaderActivity.this).ensureBook(key, title);
                } catch (IOException e) {
                    err = e.getMessage();
                }
                final String ferr = err;
                ui.post(new Runnable() {
                    @Override
                    public void run() {
                        pd.dismiss();
                        if (ferr != null) {
                            Toast.makeText(ReaderActivity.this, "取书失败：" + ferr,
                                    Toast.LENGTH_LONG).show();
                            finish();
                            return;
                        }
                        text = readFile(Store.bookFile(ReaderActivity.this, key));
                        ready = true;
                        startReading();
                    }
                });
            }
        }, "nr-fetch").start();
    }

    /** 决定从哪一页开始：本地记录优先；和电脑差得多就问一句要不要跳过去。 */
    private void startReading() {
        Store.Record local = Store.loadRecord(this, key);
        int off = (local != null) ? local.off : getIntent().getIntExtra("off", 0);
        if (off < 0) off = 0;
        if (off > text.length() - 1) off = Math.max(0, text.length() - 1);
        pageStart = alignToPageStart(off);
        render();

        /* 问一次电脑那边读到哪了（后台线程，问不到就算了） */
        new Thread(new Runnable() {
            @Override
            public void run() {
                List<Store.Record> shelf;
                try {
                    shelf = Sync.get(ReaderActivity.this).fetchShelf();
                } catch (IOException e) {
                    return;
                }
                Store.Record mine = null;
                for (Store.Record r : shelf) {
                    if (r.key.equals(key)) { mine = r; break; }
                }
                if (mine == null) return;
                final int pcOff = mine.off;
                final String pcCh = mine.chapter;
                final int pcPct = mine.pct;
                /* 只在"电脑读得更靠后"时才问。原来用 Math.abs：电脑落后时也会弹窗，
                 * 点"跳过去"反而把手机进度往回拽。C++ 侧是单向的
                 * （hit->off - mine.off <= 200），两端规则要对齐。 */
                if (pcOff - pageStart <= 200) return;
                ui.post(new Runnable() {
                    @Override
                    public void run() {
                        MainActivity.ask(ReaderActivity.this, "阅读同步",
                                "电脑的进度：" + fmtPct(pcPct) + (pcCh.isEmpty() ? "" : "　" + pcCh)
                                        + "\n手机的进度：" + fmtPct(pageStart * 1000 / Math.max(1, text.length()))
                                        + "\n\n跳到电脑那个位置继续读吗？",
                                "跳过去", "留在手机这里",
                                new DialogInterface.OnClickListener() {
                                    @Override
                                    public void onClick(DialogInterface d, int which) {
                                        pageStart = alignToPageStart(pcOff);
                                        render();
                                        report();
                                    }
                                });
                    }
                });
            }
        }, "nr-check").start();
    }

    private String fmtPct(int pct) {
        return String.format("%.1f%%", pct / 10.0);
    }

    /** 把任意位置对齐到"页首"：往前找到最近的换行，保证一页从段落开头起。 */
    private int alignToPageStart(int off) {
        if (off <= 0) return 0;
        if (off >= text.length()) return Math.max(0, text.length() - 1);
        int p = off;
        int guard = 0;
        while (p > 0 && text.charAt(p) != '\n' && guard++ < 400) p--;
        return (text.charAt(p) == '\n') ? p + 1 : off;
    }

    /** 重新算"一屏多少字"，然后画当前页。 */
    private void render() {
        if (!ready) return;

        int w = page.getWidth();
        int h = page.getHeight();
        if (w <= 0 || h <= 0) {
            /* 首帧还没量到尺寸：等下一帧再画 */
            page.post(new Runnable() {
                @Override
                public void run() { render(); }
            });
            return;
        }

        float cw = page.getPaint().measureText("汉");
        int perLine = Math.max(8, (int) (w / Math.max(1f, cw)));
        float lineH = page.getPaint().getFontMetrics().descent
                - page.getPaint().getFontMetrics().ascent + dp(8);
        int lines = Math.max(3, (int) (h / Math.max(1f, lineH)));
        charsPerPage = perLine * lines;

        int end = Math.min(text.length(), pageStart + charsPerPage);
        if (end < text.length()) {
            /* 别把一行字切成两半：往回找最近的换行（最多找 15% 页长） */
            int limit = Math.max(pageStart + 1, end - charsPerPage / 7);
            int p = end;
            while (p > limit && text.charAt(p - 1) != '\n') p--;
            if (p > limit) end = p;
        }
        int start = Math.max(0, pageStart);
        page.setText(text.substring(start, end));

        head.setText((title.isEmpty() ? "阅读" : title) + "　" + chapterAt(start));
        foot.setText(String.format("%d%%　·　第 %d / %d 页",
                (int) (start * 100L / Math.max(1, text.length())),
                start / Math.max(1, charsPerPage) + 1,
                (text.length() + charsPerPage - 1) / Math.max(1, charsPerPage)));
    }

    private void flip(int dir) {
        if (!ready) return;
        int old = pageStart;
        if (dir > 0) {
            if (pageStart + charsPerPage >= text.length()) {
                Toast.makeText(this, "已经是最后一页了", Toast.LENGTH_SHORT).show();
                return;
            }
            pageStart = alignToPageStart(pageStart + charsPerPage);
        } else {
            if (pageStart <= 0) {
                Toast.makeText(this, "已经是第一页了", Toast.LENGTH_SHORT).show();
                return;
            }
            /* 往回一页：先退一屏，再对齐到段落开头。
             * 极端情况下（整段比一屏还长）对齐会退回原处，那就硬退一屏，
             * 否则会出现"按了上一页却纹丝不动"。 */
            int back = Math.max(0, pageStart - charsPerPage);
            int aligned = alignToPageStart(back);
            pageStart = (aligned >= old) ? back : aligned;
        }
        render();
        report();
    }

    /** 上报进度：偏移 + 千分数 + 当前章名。 */
    private void report() {
        int pct = (int) (pageStart * 1000L / Math.max(1, text.length()));
        Sync.get(this).report(key, title, pageStart, pct, chapterAt(pageStart));
    }

    /** 往上找最近的一行"第 N 章"，当作当前章名（找不到就空着）。 */
    private String chapterAt(int off) {
        int from = Math.max(0, off - 6000);
        String seg = text.substring(from, Math.min(text.length(), off + 200));
        Pattern p = Pattern.compile("^\\s*(第[0-9零一二三四五六七八九十百千两]+[章节回卷集篇幕话][^\\n]{0,30})$",
                Pattern.MULTILINE);
        Matcher m = p.matcher(seg);
        String last = "";
        while (m.find()) last = m.group(1).trim();
        return last;
    }

    private String readFile(File f) {
        try {
            FileInputStream in = new FileInputStream(f);
            ByteArrayOutputStream bo = new ByteArrayOutputStream((int) Math.min(f.length(), 1 << 20));
            byte[] buf = new byte[8192];
            int n;
            while ((n = in.read(buf)) > 0) bo.write(buf, 0, n);
            in.close();
            return new String(bo.toByteArray(), "UTF-8");
        } catch (IOException e) {
            return "";
        }
    }

    private int dp(int v) {
        return (int) (v * getResources().getDisplayMetrics().density + 0.5f);
    }
}
