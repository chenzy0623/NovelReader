package com.chenzy0623.novelreader;

import android.content.Context;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.util.ArrayList;
import java.util.List;

/**
 * 同步总闸：把"发现 / 服务 / 拉书架 / 拉正文 / 上报进度"串起来的单例。
 *
 * 生命周期挂在 Application 上（进程级）：进阅读页、旋转屏幕都不重启后台线程。
 * 用户可以整体关掉——开关状态存在 SharedPreferences 的 syncOn 里，
 * 下次启动沿用，跟电脑端"由用户自己开启"对齐。
 */
final class Sync {

    private static Sync s_instance;

    static synchronized Sync get(Context c) {
        if (s_instance == null) s_instance = new Sync(c.getApplicationContext());
        return s_instance;
    }

    private static final String PREF = "sync";

    private final Context app;
    private Discovery discovery;
    private PeerServer server;
    private Thread tDisc, tSrv;
    private int myPort = 0;
    private String lastError = "";

    private Sync(Context c) {
        this.app = c;
    }

    boolean enabled() {
        return app.getSharedPreferences(PREF, Context.MODE_PRIVATE).getBoolean("syncOn", true);
    }

    void setEnabled(boolean on) {
        app.getSharedPreferences(PREF, Context.MODE_PRIVATE).edit().putBoolean("syncOn", on).apply();
        if (on) start();
        else stop();
    }

    String lastError() {
        return lastError;
    }

    synchronized void start() {
        if (tSrv != null) return;           /* 已经在跑 */
        lastError = "";

        /* 端口 8233 是约定的；万一被别的 App 占了就往后挪几位 */
        server = null;
        for (int p = 8233; p <= 8241; p++) {
            try {
                java.net.ServerSocket probe = new java.net.ServerSocket(p);
                probe.close();
                myPort = p;
                break;
            } catch (IOException ignored) {
                myPort = 0;
            }
        }
        if (myPort == 0) myPort = 8233;

        server = new PeerServer(app, myPort);
        tSrv = new Thread(server, "nr-peer-server");
        tSrv.setDaemon(true);
        tSrv.start();

        String selfId = Long.toHexString(System.nanoTime() ^ (System.currentTimeMillis() << 8));
        discovery = new Discovery(app, myPort, selfId);
        tDisc = new Thread(discovery, "nr-discovery");
        tDisc.setDaemon(true);
        tDisc.start();
    }

    synchronized void stop() {
        if (discovery != null) { discovery.stop(); discovery = null; }
        if (server != null) { server.stop(); server = null; }
        tDisc = null;
        tSrv = null;
    }

    /** 当前对端的 baseUrl（形如 http://192.168.1.5:8233）；没有就返回 null。 */
    String peerBase() {
        if (discovery == null) return null;
        Discovery.Peer p = discovery.peer();
        if (p.port == 0) return null;
        return "http://" + p.ip + ":" + p.port;
    }

    /** 拉对端的书架（每行一条记录）。网络失败抛 IOException，调用方提示即可。 */
    List<Store.Record> fetchShelf() throws IOException {
        String base = peerBase();
        if (base == null) throw new IOException("还没找到电脑，确认两边在同一个 Wi-Fi、电脑上打开了「阅读同步」");
        String body = Net.get(base + "/peer/books", 4000);
        List<Store.Record> out = new ArrayList<>();
        for (String line : body.split("\n")) {
            if (line.trim().isEmpty()) continue;
            Store.Record r = Store.Record.decode(line);
            if (r != null) out.add(r);
        }
        return out;
    }

    /** 把一本书的正文抓到本地缓存；返回缓存文件。已经缓存过就不重复下载。 */
    File ensureBook(String key, String title) throws IOException {
        File f = Store.bookFile(app, key);
        if (f.exists() && f.length() > 0) return f;

        String base = peerBase();
        if (base == null) throw new IOException("还没找到电脑");
        String body = Net.get(base + "/peer/book?key=" + Net.urlArg(key), 20000);
        int nl = body.indexOf('\n');
        if (nl < 0) throw new IOException("电脑返回的书本格式不对");
        Store.Record r = Store.Record.decode(body.substring(0, nl));
        String text = body.substring(nl + 1);
        if (text.isEmpty()) throw new IOException("这本书的正文是空的");

        FileOutputStream fo = new FileOutputStream(f);
        try {
            fo.write(text.getBytes("UTF-8"));
        } finally {
            fo.close();
        }
        if (r != null) {
            Store.saveRecord(app, r.key, r.title, r.off, r.pct, r.chapterIndex, r.chapter);
        } else if (title != null) {
            Store.saveRecord(app, key, title, 0, 0, 0, "");
        }
        return f;
    }

    /** 上报本机进度给电脑（失败静默：手机在电梯里掉线不该打断阅读）。 */
    void report(final String key, final String title, final int off, final int pct, final String chapter) {
        Store.saveRecord(app, key, title, off, pct, 0, chapter);
        final String base = peerBase();
        if (base == null) return;
        Store.Record r = Store.loadRecord(app, key);
        if (r == null) return;
        final String line = r.encode();
        new Thread(new Runnable() {
            @Override
            public void run() {
                try {
                    Net.post(base + "/peer/progress", line, 4000);
                } catch (IOException ignored) {
                    /* 网络不可达就算了，下次翻页还会再试 */
                }
            }
        }, "nr-report").start();
    }
}
