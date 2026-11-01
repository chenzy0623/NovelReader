package com.chenzy0623.novelreader;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.DialogInterface;
import android.content.Intent;
import android.graphics.Color;
import android.graphics.drawable.GradientDrawable;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.AdapterView;
import android.widget.BaseAdapter;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ListView;
import android.widget.TextView;
import android.widget.Toast;

import java.io.IOException;
import java.util.ArrayList;
import java.util.List;

/**
 * 书架页：显示电脑上的书（书名 + 读到哪一章 + 百分比），点一本就进阅读页。
 *
 * 界面为什么全用代码搭：这个 App 没有 Gradle、没有 AndroidX，
 * 连布局 XML 都尽量少（少一个文件就少一处 aapt2 出错的机会）。
 * 颜色沿用电脑端的"纸感"配色，两边的观感是一套的。
 */
public class MainActivity extends Activity {

    /* 跟电脑端一致的纸感配色 */
    private static final int C_BG    = 0xFFF7F3E9;
    private static final int C_CARD  = 0xFFFDFAF2;
    private static final int C_LINE  = 0xFFE2D9C6;
    private static final int C_FG    = 0xFF312D27;
    private static final int C_DIM   = 0xFF8C8578;
    private static final int C_TITLE = 0xFF8A5A2B;

    private final Handler ui = new Handler(Looper.getMainLooper());
    private final List<Store.Record> items = new ArrayList<>();
    private ShelfAdapter adapter;
    private TextView status;
    private Button btnSync;

    @Override
    protected void onCreate(Bundle b) {
        super.onCreate(b);

        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setBackgroundColor(C_BG);

        /* 顶部：标题 + 状态 + 同步开关 */
        LinearLayout bar = new LinearLayout(this);
        bar.setOrientation(LinearLayout.HORIZONTAL);
        bar.setGravity(Gravity.CENTER_VERTICAL);
        bar.setPadding(dp(16), dp(12), dp(12), dp(8));

        TextView title = new TextView(this);
        title.setText("小说阅读器");
        title.setTextColor(C_TITLE);
        title.setTextSize(19);
        bar.addView(title, new LinearLayout.LayoutParams(0,
                ViewGroup.LayoutParams.WRAP_CONTENT, 1f));

        btnSync = new Button(this);
        btnSync.setText("同步");
        btnSync.setTextSize(13);
        btnSync.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                Sync s = Sync.get(MainActivity.this);
                boolean on = !s.enabled();
                s.setEnabled(on);
                Toast.makeText(MainActivity.this,
                        on ? "同步已开启，正在找电脑…" : "同步已关闭", Toast.LENGTH_SHORT).show();
                refresh();
            }
        });
        bar.addView(btnSync);
        root.addView(bar);

        status = new TextView(this);
        status.setTextColor(C_DIM);
        status.setTextSize(13);
        status.setPadding(dp(16), 0, dp(16), dp(8));
        root.addView(status);

        ListView list = new ListView(this);
        list.setBackgroundColor(C_BG);
        list.setDivider(null);
        list.setDividerHeight(0);
        adapter = new ShelfAdapter();
        list.setAdapter(adapter);
        list.setOnItemClickListener(new AdapterView.OnItemClickListener() {
            @Override
            public void onItemClick(AdapterView<?> parent, View view, int pos, long id) {
                openBook(items.get(pos));
            }
        });
        root.addView(list, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f));

        setContentView(root);

        Sync s = Sync.get(this);
        if (s.enabled() && s.peerBase() == null) s.start();
        refresh();
    }

    @Override
    protected void onResume() {
        super.onResume();
        refresh();
    }

    /** 拉一次书架（后台线程），回来再刷列表。 */
    private void refresh() {
        final Sync s = Sync.get(this);
        btnSync.setText(s.enabled() ? "同步中" : "同步已关");
        status.setText(s.peerBase() == null
                ? (s.enabled() ? "正在局域网里找电脑…（电脑上也要打开「阅读同步」）"
                               : "同步已关闭：只能看已下载到手机上的书")
                : "已连接：" + s.peerBase());

        new Thread(new Runnable() {
            @Override
            public void run() {
                final List<Store.Record> got = new ArrayList<>();
                String err = null;
                if (s.enabled()) {
                    try {
                        got.addAll(s.fetchShelf());
                    } catch (IOException e) {
                        err = e.getMessage();
                    }
                }
                if (got.isEmpty()) {
                    /* 连不上电脑时退回本地记录：至少能继续看缓存过的书 */
                    got.addAll(Store.allRecords(MainActivity.this));
                }
                final String ferr = err;
                ui.post(new Runnable() {
                    @Override
                    public void run() {
                        items.clear();
                        items.addAll(got);
                        /* 最近读的排前面 */
                        java.util.Collections.sort(items, new java.util.Comparator<Store.Record>() {
                            @Override
                            public int compare(Store.Record a, Store.Record b) {
                                return b.ts - a.ts;
                            }
                        });
                        adapter.notifyDataSetChanged();
                        if (items.isEmpty()) {
                            status.setText(ferr != null ? ("连不上电脑：" + ferr)
                                    : "电脑上还没有在读的书。先在电脑上打开一本小说再来。");
                        }
                    }
                });
            }
        }, "nr-shelf").start();
    }

    private void openBook(Store.Record r) {
        Intent it = new Intent(this, ReaderActivity.class);
        it.putExtra("key", r.key);
        it.putExtra("title", r.title);
        it.putExtra("off", r.off);
        startActivity(it);
    }

    private int dp(int v) {
        return (int) (v * getResources().getDisplayMetrics().density + 0.5f);
    }

    /** 每本书一张卡片：书名 + 章名 + 百分比 + 一条细进度条。 */
    private final class ShelfAdapter extends BaseAdapter {
        @Override
        public int getCount() { return items.size(); }

        @Override
        public Object getItem(int i) { return items.get(i); }

        @Override
        public long getItemId(int i) { return i; }

        @Override
        public View getView(int i, View reuse, ViewGroup parent) {
            LinearLayout box;
            TextView name, sub;
            if (reuse instanceof LinearLayout) {
                box = (LinearLayout) reuse;
                name = (TextView) box.getChildAt(0);
                sub = (TextView) box.getChildAt(1);
            } else {
                box = new LinearLayout(MainActivity.this);
                box.setOrientation(LinearLayout.VERTICAL);
                box.setPadding(dp(16), dp(12), dp(16), dp(14));

                GradientDrawable bg = new GradientDrawable();
                bg.setColor(C_CARD);
                bg.setCornerRadius(dp(10));
                bg.setStroke(dp(1), C_LINE);
                box.setBackground(bg);

                name = new TextView(MainActivity.this);
                name.setTextColor(C_FG);
                name.setTextSize(17);
                box.addView(name);

                sub = new TextView(MainActivity.this);
                sub.setTextColor(C_DIM);
                sub.setTextSize(13);
                sub.setPadding(0, dp(4), 0, 0);
                box.addView(sub);
            }

            Store.Record r = items.get(i);
            name.setText(r.title.isEmpty() ? r.key : r.title);
            String ch = r.chapter.isEmpty() ? "还没开始读" : r.chapter;
            sub.setText(String.format("%s　·　%.1f%%", ch, r.pct / 10.0));
            return box;
        }
    }

    /** 一个通用的"问一句"对话框（和电脑端的主题化消息框对应）。 */
    static void ask(Activity a, String title, String msg, String yes, String no,
                    DialogInterface.OnClickListener onYes) {
        new AlertDialog.Builder(a)
                .setTitle(title)
                .setMessage(msg)
                .setPositiveButton(yes, onYes)
                .setNegativeButton(no, null)
                .show();
    }
}
