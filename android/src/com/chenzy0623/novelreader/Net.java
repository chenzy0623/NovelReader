package com.chenzy0623.novelreader;

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.io.UnsupportedEncodingException;
import java.net.HttpURLConnection;
import java.net.URL;
import java.net.URLEncoder;

/**
 * 极简 HTTP 客户端 + 协议的百分号转义。
 *
 * 为什么不用 OkHttp：这个 App 一共就四种请求，用 HttpURLConnection 足够，
 * 少一个依赖就少几 MB，也让"手写构建脚本"这条路走得通（没有 Gradle 就没有依赖解析）。
 */
final class Net {

    private Net() { }

    /** 一次 GET，返回正文；失败抛异常（调用方负责提示）。 */
    static String get(String url, int timeoutMs) throws IOException {
        HttpURLConnection c = (HttpURLConnection) new URL(url).openConnection();
        c.setConnectTimeout(timeoutMs);
        c.setReadTimeout(timeoutMs);
        c.setRequestMethod("GET");
        c.setRequestProperty("Connection", "close");
        try {
            int code = c.getResponseCode();
            if (code != 200) throw new IOException("HTTP " + code);
            return readAll(c.getInputStream());
        } finally {
            c.disconnect();
        }
    }

    /** 一次 POST（正文是纯文本），返回响应正文。 */
    static String post(String url, String body, int timeoutMs) throws IOException {
        HttpURLConnection c = (HttpURLConnection) new URL(url).openConnection();
        c.setConnectTimeout(timeoutMs);
        c.setReadTimeout(timeoutMs);
        c.setRequestMethod("POST");
        c.setRequestProperty("Connection", "close");
        c.setRequestProperty("Content-Type", "text/plain; charset=utf-8");
        c.setDoOutput(true);
        byte[] raw = body.getBytes("UTF-8");
        c.setFixedLengthStreamingMode(raw.length);
        try {
            OutputStream os = c.getOutputStream();
            os.write(raw);
            os.flush();
            os.close();
            int code = c.getResponseCode();
            if (code != 200) throw new IOException("HTTP " + code);
            return readAll(c.getInputStream());
        } finally {
            c.disconnect();
        }
    }

    private static String readAll(InputStream in) throws IOException {
        ByteArrayOutputStream bo = new ByteArrayOutputStream();
        byte[] buf = new byte[8192];
        int n;
        while ((n = in.read(buf)) > 0) bo.write(buf, 0, n);
        in.close();
        return new String(bo.toByteArray(), "UTF-8");
    }

    /* ---------------- 协议的百分号转义 ----------------
     * 只转义会破坏"一行一条记录"的四个字符（制表符、换行、回车、百分号本身），
     * 其它一律原样——这样书本名里出现中文、空格、斜杠都不用管。 */

    static String pct(String s) {
        if (s == null) return "";
        StringBuilder sb = new StringBuilder(s.length() + 8);
        for (int i = 0; i < s.length(); i++) {
            char ch = s.charAt(i);
            switch (ch) {
                case '%':  sb.append("%25"); break;
                case '\t': sb.append("%09"); break;
                case '\n': sb.append("%0A"); break;
                case '\r': sb.append("%0D"); break;
                default:   sb.append(ch);
            }
        }
        return sb.toString();
    }

    static String unpct(String s) {
        if (s == null) return "";
        StringBuilder sb = new StringBuilder(s.length());
        for (int i = 0; i < s.length(); i++) {
            char ch = s.charAt(i);
            if (ch == '%' && i + 2 < s.length()) {
                try {
                    sb.append((char) Integer.parseInt(s.substring(i + 1, i + 3), 16));
                    i += 2;
                    continue;
                } catch (NumberFormatException ignored) {
                    /* 不是合法转义就原样输出，别把整条记录弄坏 */
                }
            }
            sb.append(ch);
        }
        return sb.toString();
    }

    /** 把查询参数做 URL 编码（这里只需要处理 key，它本来就是十六进制）。 */
    static String urlArg(String s) {
        try {
            return URLEncoder.encode(s, "UTF-8");
        } catch (UnsupportedEncodingException e) {
            return s;
        }
    }
}
