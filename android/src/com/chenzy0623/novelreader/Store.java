package com.chenzy0623.novelreader;

import android.content.Context;
import android.content.SharedPreferences;

import java.io.File;
import java.util.ArrayList;
import java.util.List;

/**
 * 本机存储：阅读记录 + 正文缓存。
 *
 * 为什么记录要自己存一份：
 *   PeerServer 需要把手机上的进度回答给电脑（电脑一开书就来问），
 *   所以记录不能只放在内存里，必须落盘。
 *
 * 记录格式与协议完全一致（一行一条，制表符分隔，字段做百分号转义）：
 *   key \t title \t off \t pct \t ts \t chapter
 */
final class Store {

    private static final String PREF = "records";

    private Store() { }

    private static SharedPreferences prefs(Context c) {
        return c.getSharedPreferences(PREF, Context.MODE_PRIVATE);
    }

    /** 保存一条记录（key 是书的指纹，由电脑端算好发过来，手机只负责记住）。 */
    static void saveRecord(Context c, String key, String title, int off, int pct, int chapter, String chapterName) {
        if (key == null || key.isEmpty()) return;
        Record r = new Record();
        r.key = key;
        r.title = title == null ? "" : title;
        r.off = off;
        r.pct = pct;
        r.ts = (int) (System.currentTimeMillis() / 1000L);
        r.chapter = chapterName == null ? "" : chapterName;
        r.chapterIndex = chapter;
        prefs(c).edit().putString(key, r.encode()).apply();
    }

    /** 读一条记录；没有就返回 null。 */
    static Record loadRecord(Context c, String key) {
        String s = prefs(c).getString(key, null);
        if (s == null) return null;
        return Record.decode(s);
    }

    /** 所有记录（给 /peer/books 用，也用来画书架）。 */
    static List<Record> allRecords(Context c) {
        List<Record> out = new ArrayList<>();
        for (String k : prefs(c).getAll().keySet()) {
            String v = prefs(c).getString(k, null);
            if (v == null) continue;
            Record r = Record.decode(v);
            if (r != null && !r.key.isEmpty()) out.add(r);
        }
        return out;
    }

    /** 正文缓存文件（一本书一个，文件名就是指纹，避免中文文件名的问题）。 */
    static File bookFile(Context c, String key) {
        File dir = new File(c.getFilesDir(), "books");
        if (!dir.exists()) dir.mkdirs();
        return new File(dir, key + ".txt");
    }

    /** 一条阅读记录。字段含义和电脑端完全对齐。 */
    static final class Record {
        String key = "";
        String title = "";
        int off = 0;            /* 读到第几个字符 */
        int pct = 0;            /* 千分数，0~1000 */
        int ts = 0;             /* 最后更新时间（Unix 秒） */
        int chapterIndex = 0;
        String chapter = "";

        String encode() {
            return Net.pct(key) + "\t" + Net.pct(title) + "\t" + off + "\t" + pct + "\t" + ts
                    + "\t" + Net.pct(chapter);
        }

        static Record decode(String line) {
            if (line == null) return null;
            String[] f = line.split("\t", -1);
            if (f.length < 5) return null;
            Record r = new Record();
            try {
                r.key = Net.unpct(f[0]);
                r.title = Net.unpct(f[1]);
                r.off = Integer.parseInt(f[2].trim());
                r.pct = Integer.parseInt(f[3].trim());
                r.ts = Integer.parseInt(f[4].trim());
                if (f.length > 5) r.chapter = Net.unpct(f[5]);
            } catch (NumberFormatException e) {
                return null;
            }
            return r;
        }
    }
}
