#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
test_serve_music.py —— serve_music.py 的自测脚本（不需要 ESP32）

运行：
    python tools/test_serve_music.py

它检查两件事：
  1. 播放列表的生成（相对地址 / 绝对地址、百分号编码、#EXTINF 标题）
  2. HTTP Range 的支持（200 / 206 / 416）—— 这正是 ESP32 断点续传需要的
"""

import os                # 路径、目录操作
import sys               # 退出码、sys.path
import threading         # 后台线程跑服务器
import urllib.error      # HTTP 错误响应
import urllib.request    # 发 HTTP 请求

# 把本目录加进模块搜索路径，才能 import 旁边的 serve_music
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import serve_music as sm

# ---- 造两个临时 mp3 文件（内容不重要，长度要不同，方便验证 Range） ----
folder = os.path.join(os.environ.get("TEMP", "/tmp"), "rfm_music")
os.makedirs(folder, exist_ok=True)                             # 确保临时目录存在
open(os.path.join(folder, "01 晴天.mp3"), "wb").write(b"x" * 1000)   # 1000 字节
open(os.path.join(folder, "02 稻香.mp3"), "wb").write(b"y" * 2000)   # 2000 字节

# ---- 在本地临时端口上起一个服务器（后台线程） ----
h = lambda *a, **kw: sm.RangeHandler(*a, directory=folder, **kw)
srv = sm.Server(("127.0.0.1", 8123), h)
threading.Thread(target=srv.serve_forever, daemon=True).start()

failed = []          # 记录失败的检查项


def check(label, cond, extra=""):
    """打印一条检查结果，失败时记进 failed 列表。"""
    print(("  ok   " if cond else "  FAIL ") + label + ("  " + extra if extra else ""))
    if not cond:
        failed.append(label)


def get(path, headers=None):
    """发一个 GET 请求，返回 (状态码, 响应头字典, 响应体)。"""
    req = urllib.request.Request("http://127.0.0.1:8123" + path, headers=headers or {})
    r = urllib.request.urlopen(req)
    return r.status, dict(r.headers), r.read()


# ===================== 播放列表 =====================
print("playlists")
s, hd, body = get("/list.m3u")                          # 取相对地址歌单
text = body.decode("utf-8")
check("list.m3u status 200", s == 200)                   # 状态码要对
check("list.m3u content type", hd.get("Content-Type") == "audio/x-mpegurl")
check("list.m3u starts with #EXTM3U", text.startswith("#EXTM3U"))
check("list.m3u has both tracks", text.count(".mp3") == 2, repr(text))
check("list.m3u entries are url encoded", "%20" in text, repr(text))  # 空格要编码
check("list.m3u has #EXTINF titles", "#EXTINF:" in text)

s, hd, body = get("/list_abs.m3u")                       # 取绝对地址歌单
text = body.decode("utf-8")
check("list_abs.m3u status 200", s == 200)
check("list_abs.m3u has absolute urls", "http://127.0.0.1:8123/" in text, repr(text))

# ===================== Range 请求 =====================
print("range requests")
path = "/02%20%E7%A8%BB%E9%A6%99.mp3"                    # 百分号编码后的文件名
s, hd, body = get(path)                                  # 整个文件
check("full file 200", s == 200)
check("full file length", len(body) == 2000, str(len(body)))
check("advertises accept-ranges", hd.get("Accept-Ranges") == "bytes")

s, hd, body = get(path, {"Range": "bytes=500-"})         # 从 500 字节开始要
check("range 500- gives 206", s == 206, str(s))
check("range content-range header", hd.get("Content-Range") == "bytes 500-1999/2000",
      str(hd.get("Content-Range")))
check("range body length", len(body) == 1500, str(len(body)))

try:
    get(path, {"Range": "bytes=99999-"})                # 越界：应该报 416
    check("range beyond eof gives 416", False)
except urllib.error.HTTPError as e:
    check("range beyond eof gives 416", e.code == 416, str(e.code))

srv.shutdown()                                           # 收工，关闭服务器
print()
if failed:
    print("FAILED: " + ", ".join(failed))                # 有失败项就以退出码 1 结束
    sys.exit(1)
print("ALL OK")
