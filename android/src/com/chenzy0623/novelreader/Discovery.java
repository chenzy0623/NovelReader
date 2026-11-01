package com.chenzy0623.novelreader;

import android.content.Context;
import android.net.wifi.WifiManager;

import java.io.IOException;
import java.net.DatagramPacket;
import java.net.DatagramSocket;
import java.net.InetAddress;
import java.net.SocketTimeoutException;

/**
 * 局域网发现：手机和电脑都不当"服务器"，双方各自定时广播一句"我在，我的端口是 N"。
 *
 * 报文格式（和电脑端 src/sync.cpp 完全一致）：
 *   NR1|<tcpPort>|phone|<selfId>
 *
 * 为什么要自报 selfId：广播会被自己收到（同一台机器上很容易发生），
 * 拿自己的 id 一比就知道该丢掉，不然会连到自己、进度差永远是 0。
 */
final class Discovery implements Runnable {

    static final int UDP_PORT = 8234;

    /** 对端信息：IP + 端口。 */
    static final class Peer {
        String ip = "";
        int port = 0;
        int seenAt = 0;         /* 最后一次收到广播的时刻（毫秒） */
    }

    private final Context app;
    private final int myTcpPort;
    private final String selfId;
    private volatile boolean running = true;
    private volatile Peer peer = new Peer();
    private WifiManager.MulticastLock lock;

    Discovery(Context c, int tcpPort, String selfId) {
        this.app = c.getApplicationContext();
        this.myTcpPort = tcpPort;
        this.selfId = selfId;
    }

    Peer peer() {
        /* 15 秒没动静就当离线，和电脑端一致 */
        Peer p = peer;
        if (p.port != 0 && System.currentTimeMillis() - p.seenAt > 15000) {
            Peer dead = new Peer();
            peer = dead;
            return dead;
        }
        return p;
    }

    void stop() {
        running = false;
    }

    @Override
    public void run() {
        /* 很多机型默认把广播包过滤掉，必须持有组播锁才收得到 */
        try {
            WifiManager wm = (WifiManager) app.getApplicationContext()
                    .getSystemService(Context.WIFI_SERVICE);
            if (wm != null) {
                lock = wm.createMulticastLock("novelreader-sync");
                lock.setReferenceCounted(false);
                lock.acquire();
            }
        } catch (Exception ignored) {
            /* 拿不到锁也继续试：有些系统本来就不需要 */
        }

        DatagramSocket sock = null;
        try {
            sock = new DatagramSocket(UDP_PORT);
            sock.setBroadcast(true);
            sock.setSoTimeout(2000);

            byte[] hello = ("NR1|" + myTcpPort + "|phone|" + selfId).getBytes("UTF-8");
            byte[] buf = new byte[512];
            long lastSend = 0;

            while (running) {
                long now = System.currentTimeMillis();
                if (now - lastSend >= 2000) {
                    lastSend = now;
                    try {
                        sock.send(new DatagramPacket(hello, hello.length,
                                InetAddress.getByName("255.255.255.255"), UDP_PORT));
                    } catch (IOException ignored) {
                        /* 广播失败不影响接收，下一轮再试 */
                    }
                }

                DatagramPacket pkt = new DatagramPacket(buf, buf.length);
                try {
                    sock.receive(pkt);
                } catch (SocketTimeoutException te) {
                    continue;
                } catch (IOException ioe) {
                    break;
                }

                String text = new String(pkt.getData(), 0, pkt.getLength(), "UTF-8");
                String[] f = text.split("\\|", -1);
                if (f.length < 4 || !"NR1".equals(f[0])) continue;
                int port;
                try {
                    port = Integer.parseInt(f[1].trim());
                } catch (NumberFormatException e) {
                    continue;
                }
                /* 自己的包：丢掉；电脑端名字是 pc，手机是 phone，认不出来的也接受 */
                if (selfId.equals(f[3].trim())) continue;
                if (port <= 0 || port > 65535) continue;

                Peer p = new Peer();
                p.ip = pkt.getAddress().getHostAddress();
                p.port = port;
                p.seenAt = (int) (System.currentTimeMillis() & 0x7fffffff);
                peer = p;
            }
        } catch (IOException e) {
            /* 端口被占或权限问题：让上层显示"找不到电脑"，不要崩 */
        } finally {
            if (sock != null) sock.close();
            if (lock != null && lock.isHeld()) lock.release();
        }
    }
}
