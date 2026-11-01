package com.chenzy0623.novelreader;

import android.content.Context;

import java.io.BufferedReader;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.io.OutputStream;
import java.net.ServerSocket;
import java.net.Socket;
import java.net.URLDecoder;
import java.util.List;

/**
 * 手机这一侧的 HTTP 服务：只服务电脑（同一个 Wi-Fi），一共四个接口。
 *
 *   GET  /peer/ping          → "NRPEER 1"
 *   GET  /peer/books         → 每行一条记录（书架 + 进度）
 *   GET  /peer/book?key=xxx  → 第一行是记录，其后是正文全文
 *   POST /peer/progress      → 正文就是一行记录，收到就落盘
 *
 * 为什么要手机也开服务：电脑那边问"你读到哪了"必须有个东西回答。
 * 双方对等（都不当固定服务端），谁先开都能问到对方。
 */
final class PeerServer implements Runnable {

    private final Context app;
    private final int port;
    private volatile boolean running = true;

    PeerServer(Context c, int port) {
        this.app = c.getApplicationContext();
        this.port = port;
    }

    void stop() {
        running = false;
    }

    @Override
    public void run() {
        ServerSocket ss = null;
        try {
            ss = new ServerSocket(port);
            ss.setSoTimeout(1000);
            while (running) {
                Socket s = null;
                try {
                    s = ss.accept();
                    handle(s);
                } catch (java.net.SocketTimeoutException te) {
                    continue;
                } catch (IOException ioe) {
                    /* 单个连接出错不要停整个服务 */
                } finally {
                    if (s != null) try { s.close(); } catch (IOException ignored) { }
                }
            }
        } catch (IOException e) {
            /* 端口被占用：上层会显示"手机端服务没起来"，电脑仍然可以主动连手机？不行，
             * 但至少不要崩。 */
        } finally {
            if (ss != null) try { ss.close(); } catch (IOException ignored) { }
        }
    }

    private void handle(Socket s) throws IOException {
        s.setSoTimeout(5000);
        BufferedReader in = new BufferedReader(new InputStreamReader(s.getInputStream(), "UTF-8"));

        String request = in.readLine();
        if (request == null) return;
        int contentLength = 0;
        String line;
        while ((line = in.readLine()) != null && !line.isEmpty()) {
            int c = line.indexOf(':');
            if (c > 0 && line.substring(0, c).trim().equalsIgnoreCase("Content-Length")) {
                try {
                    contentLength = Integer.parseInt(line.substring(c + 1).trim());
                } catch (NumberFormatException ignored) { }
            }
        }

        String[] parts = request.split(" ");
        if (parts.length < 2) return;
        String method = parts[0];
        String path = parts[1];

        if (path.startsWith("/peer/ping")) {
            reply(s, 200, "text/plain; charset=utf-8", "NRPEER 1\n");
            return;
        }

        if (path.startsWith("/peer/books")) {
            StringBuilder sb = new StringBuilder();
            List<Store.Record> all = Store.allRecords(app);
            for (Store.Record r : all) {
                sb.append(r.encode()).append('\n');
            }
            reply(s, 200, "text/plain; charset=utf-8", sb.toString());
            return;
        }

        if (path.startsWith("/peer/book")) {
            String key = queryParam(path, "key");
            if (key == null || key.isEmpty()) {
                reply(s, 404, "text/plain; charset=utf-8", "no key\n");
                return;
            }
            Store.Record r = Store.loadRecord(app, key);
            File f = Store.bookFile(app, key);
            if (r == null || !f.exists()) {
                reply(s, 404, "text/plain; charset=utf-8", "no book\n");
                return;
            }
            byte[] head = (r.encode() + "\n").getBytes("UTF-8");
            byte[] body = readFile(f);
            ByteArrayOutputStream bo = new ByteArrayOutputStream(head.length + body.length);
            bo.write(head);
            bo.write(body);
            reply(s, 200, "text/plain; charset=utf-8", bo.toByteArray());
            return;
        }

        if (path.startsWith("/peer/progress") && "POST".equals(method)) {
            /* 必须从 in 里读：BufferedReader 为了找换行已经把 body 一起读进它自己的
             * 缓冲区了，再从 s.getInputStream() 读原始流只会读到 0 字节 ——
             * 原来这里就是这样：电脑推过来的进度全丢、永远回 400。 */
            StringBuilder sb = new StringBuilder();
            int got = 0;
            while (got < contentLength) {
                int ch = in.read();
                if (ch < 0) break;
                sb.append((char) ch);
                got += String.valueOf((char) ch).getBytes("UTF-8").length;
            }
            String text = sb.toString().trim();
            Store.Record r = Store.Record.decode(text);
            if (r == null) {
                reply(s, 400, "text/plain; charset=utf-8", "bad record\n");
                return;
            }
            Store.saveRecord(app, r.key, r.title, r.off, r.pct, r.chapterIndex, r.chapter);
            reply(s, 200, "text/plain; charset=utf-8", "ok\n");
            return;
        }

        reply(s, 404, "text/plain; charset=utf-8", "not found\n");
    }

    private static String queryParam(String path, String name) {
        int q = path.indexOf('?');
        if (q < 0) return null;
        String[] pairs = path.substring(q + 1).split("&");
        for (String p : pairs) {
            int e = p.indexOf('=');
            if (e < 0) continue;
            if (p.substring(0, e).equals(name)) {
                try {
                    return URLDecoder.decode(p.substring(e + 1), "UTF-8");
                } catch (Exception ex) {
                    return p.substring(e + 1);
                }
            }
        }
        return null;
    }

    private static byte[] readFile(File f) throws IOException {
        FileInputStream in = new FileInputStream(f);
        try {
            ByteArrayOutputStream bo = new ByteArrayOutputStream((int) Math.min(f.length(), 1 << 20));
            byte[] buf = new byte[8192];
            int n;
            while ((n = in.read(buf)) > 0) bo.write(buf, 0, n);
            return bo.toByteArray();
        } finally {
            in.close();
        }
    }

    private static void reply(Socket s, int code, String type, String body) throws IOException {
        reply(s, code, type, body.getBytes("UTF-8"));
    }

    private static void reply(Socket s, int code, String type, byte[] body) throws IOException {
        String reason = (code == 200) ? "OK" : (code == 404 ? "Not Found" : "Bad Request");
        String head = "HTTP/1.1 " + code + " " + reason + "\r\n"
                + "Content-Type: " + type + "\r\n"
                + "Content-Length: " + body.length + "\r\n"
                + "Connection: close\r\n\r\n";
        OutputStream os = s.getOutputStream();
        os.write(head.getBytes("UTF-8"));
        os.write(body);
        os.flush();
    }
}
