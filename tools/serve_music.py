#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
serve_music.py —— 把电脑上的音乐文件夹变成 ESP32 收音机能播的 HTTP 服务

用法
----
    python tools/serve_music.py D:\\我的音乐 --port 8000

然后在 ESP32 的串口控制台里：

    url 0 http://<电脑IP>:8000/list.m3u
    play 0

它提供什么
----------
* /list.m3u      —— 播放列表，条目是【相对地址】
                    （ESP32 按播放列表所在目录去解析，换个 IP 也能用）
* /list_abs.m3u  —— 播放列表，条目是【绝对地址】
* 支持 HTTP Range（Accept-Ranges / 206 Partial Content）
                    —— ESP32 才能"断点续传"，下到一半断了不用从头再来
* .mp3 / .m3u 返回正确的 Content-Type，ESP32 才分得清是歌单还是音频流

只依赖 Python 标准库，不需要装任何东西。
"""

import argparse          # 命令行参数解析
import http.server       # HTTP 服务器基类
import os                # 路径、文件操作
import socket            # 用来探测本机 IP
import socketserver      # 多线程 TCP 服务器
import sys               # 标准错误输出、退出码


def local_ip() -> str:
    """尽力猜出别的设备能访问到的本机地址。

    做法是建一个 UDP socket 连一下公网地址（不会真的发包），
    然后看系统分配给这个连接的本端地址是什么。
    猜不到就退回 127.0.0.1。
    """
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("8.8.8.8", 80))          # 只是让系统选路，不会真的通信
        return s.getsockname()[0]           # 系统分配给这个连接的本机 IP
    except OSError:
        return "127.0.0.1"                  # 没有网络时只能用回环地址
    finally:
        s.close()                           # 无论如何都把 socket 释放掉


def urllib_quote(name: str) -> str:
    """把文件名做 URL 百分号编码。

    空格和中文在 HTTP 请求行里是非法字符，必须编码成
    %20 / %E6%99%B4 这样的形式，ESP32 才能正确请求到文件。
    """
    from urllib.parse import quote
    return quote(name)


class RangeHandler(http.server.SimpleHTTPRequestHandler):
    """在标准静态文件服务器的基础上加上 Range（断点续传）支持。"""

    # 在标准映射表基础上补充我们需要的类型
    extensions_map = {
        **http.server.SimpleHTTPRequestHandler.extensions_map,
        ".mp3": "audio/mpeg",           # MP3 音频
        ".m3u": "audio/x-mpegurl",      # m3u 播放列表
        ".m3u8": "audio/x-mpegurl",     # m3u8（本工程按普通文本歌单处理）
    }

    def send_head(self):
        """发送响应头并打开文件。

        标准实现不支持 Range，这里自己实现一遍：
        没有 Range 头 -> 200 + 整个文件；
        有 Range 头   -> 206 + 从指定偏移开始的一段。
        """
        path = self.translate_path(self.path)      # URL 路径 -> 本地文件路径
        if os.path.isdir(path):                    # 目录交给标准实现（列目录）
            return super().send_head()

        try:
            f = open(path, "rb")                   # 二进制打开（音频不能做文本转换）
        except OSError:
            self.send_error(404, "File not found") # 文件不存在
            return None

        size = os.fstat(f.fileno()).st_size       # 文件总长度
        start, end, status = 0, size - 1, 200      # 默认：整个文件、状态码 200

        rng = self.headers.get("Range")            # 取客户端要的字节范围
        if rng and rng.startswith("bytes="):
            spec = rng[6:].split(",")[0].split("-")[0]   # "bytes=1000-" -> "1000"
            try:
                start = int(spec)                  # 解析起始偏移
            except ValueError:
                start = 0                          # 解析不了就当没有 Range
            if start >= size:                      # 越界：返回 416
                f.close()
                self.send_response(416, "Requested Range Not Satisfiable")
                self.send_header("Content-Range", f"bytes */{size}")
                self.end_headers()
                return None
            status = 206                           # 206 = Partial Content

        self.send_response(status)                 # HTTP 状态行
        self.send_header("Content-Type", self.guess_type(path))   # 文件类型
        self.send_header("Accept-Ranges", "bytes")                # 声明支持 Range
        self.send_header("Content-Length", str(end - start + 1))  # 本次返回多少字节
        if status == 206:
            # 206 必须带 Content-Range，告诉客户端这是整个文件的哪一段
            self.send_header("Content-Range", f"bytes {start}-{end}/{size}")
        self.end_headers()
        f.seek(start)                             # 定位到起始偏移
        return f                                  # 返回文件对象，交给标准逻辑去发

    # ---------------- 播放列表 ----------------

    def _mp3_files(self):
        """列出目录里的 .mp3 文件（按名字排序）。"""
        try:
            names = os.listdir(self.directory)
        except OSError:
            return []
        return sorted(n for n in names if n.lower().endswith(".mp3"))

    def _playlist_body(self, absolute: bool) -> bytes:
        """生成播放列表的文本内容。

        @param absolute True -> 条目写成完整 URL；False -> 只写文件名（相对）
        """
        host = self.headers.get("Host", f"{local_ip()}:{self.server.server_address[1]}")
        lines = ["#EXTM3U"]                       # m3u 的固定头
        for n in self._mp3_files():
            title = os.path.splitext(n)[0]        # 去掉扩展名当歌名
            lines.append(f"#EXTINF:-1,{title}")   # -1 = 时长未知
            if absolute:
                lines.append(f"http://{host}/{urllib_quote(n)}")
            else:
                lines.append(urllib_quote(n))    # 相对地址 + 百分号编码
        return ("\n".join(lines) + "\n").encode("utf-8", "replace")

    def _serve_playlist(self, absolute: bool):
        """把生成好的播放列表发给客户端。"""
        body = self._playlist_body(absolute)
        self.send_response(200)
        self.send_header("Content-Type", "audio/x-mpegurl")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")  # 不缓存，改了歌单立刻生效
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        """处理 GET 请求：先看是不是我们生成的播放列表。"""
        path = self.path.split("?")[0]            # 去掉查询参数
        if path == "/list.m3u":
            self._serve_playlist(absolute=False)  # 相对地址歌单
        elif path == "/list_abs.m3u":
            self._serve_playlist(absolute=True)   # 绝对地址歌单
        else:
            super().do_GET()                      # 其它路径走标准静态文件逻辑


class Server(socketserver.ThreadingTCPServer):
    """多线程 HTTP 服务器：每个连接一个线程，连接多时也不会互相拖死。"""

    allow_reuse_address = True   # 允许立刻重新绑定同一个端口（避免 TIME_WAIT 报错）
    daemon_threads = True        # 线程是守护线程，主程序退出时一起结束


def main() -> int:
    """命令行入口：解析参数、启动服务器、打印用法提示。"""
    ap = argparse.ArgumentParser(
        description=__doc__,                      # 用文件头当帮助文字
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("directory", help="存放 .mp3 文件的文件夹")
    ap.add_argument("--port", type=int, default=8000, help="监听端口，默认 8000")
    args = ap.parse_args()                       # 解析命令行

    folder = os.path.abspath(args.directory)      # 变成绝对路径
    if not os.path.isdir(folder):                 # 目录不存在就直接报错退出
        print(f"error: {folder} is not a directory", file=sys.stderr)
        return 1

    mp3s = [n for n in os.listdir(folder) if n.lower().endswith(".mp3")]
    ip = local_ip()                               # 本机可被访问的地址

    # 用 lambda 把歌曲文件夹固定给 handler（标准库的构造器只收目录参数）
    handler = lambda *a, **kw: RangeHandler(*a, directory=folder, **kw)
    with Server(("0.0.0.0", args.port), handler) as httpd:
        print("=" * 62)
        print(f"  serving   {folder}")            # 正在提供服务的目录
        print(f"  mp3 files {len(mp3s)}" + ("" if mp3s else "   (none - add some!)"))
        print("-" * 62)
        print(f"  playlist  http://{ip}:{args.port}/list.m3u      (relative entries)")
        print(f"  playlist  http://{ip}:{args.port}/list_abs.m3u  (absolute urls)")
        print("-" * 62)
        print("  on the esp32 (serial console):")
        print(f"     url 0 http://{ip}:{args.port}/list.m3u")
        print("     play 0")
        print("     pl          -> show the parsed playlist")
        print("=" * 62)
        print("  ctrl-c to stop")
        try:
            httpd.serve_forever()                # 进入服务循环
        except KeyboardInterrupt:
            print("\nbye")                        # Ctrl-C 退出
    return 0


if __name__ == "__main__":
    sys.exit(main())                              # 把返回值作为进程退出码
