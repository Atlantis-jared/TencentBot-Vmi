import cv2
import dxcam
from flask import Flask, Response, render_template_string
import time

app = Flask(__name__)

# 初始化 dxcam 截图对象 (使用 DXGI Desktop Duplication)
# target_fps 设置为你想要的捕获帧率，最高可达 60~120 但会增加网络带宽和 CPU 负载
camera = dxcam.create(output_color="BGR")

def generate_frames():
    # 开始持续捕获，设置目标帧率为 60
    camera.start(target_fps=60, video_mode=True)
    
    try:
        while True:
            # 获取最新的一帧
            frame = camera.get_latest_frame()
            if frame is None:
                continue

            # 调整分辨率以降低网络传输带宽 (可选：如果你的内网带宽足够，可以不缩放或调高分辨率)
            # 这里默认将画面缩放到 1280x720 以保证极其流畅的传输
            frame_resized = cv2.resize(frame, (1280, 720), interpolation=cv2.INTER_LINEAR)

            # 将捕获的图像帧编码为 JPEG 格式
            # quality 设置为 70 是一个比较好的画质和带宽平衡点
            ret, buffer = cv2.imencode('.jpg', frame_resized, [int(cv2.IMWRITE_JPEG_QUALITY), 70])
            if not ret:
                continue
                
            frame_bytes = buffer.tobytes()

            # 使用 multipart/x-mixed-replace 格式返回，浏览器会自动将其渲染为连续的视频流
            yield (b'--frame\r\n'
                   b'Content-Type: image/jpeg\r\n\r\n' + frame_bytes + b'\r\n')
    finally:
        camera.stop()

@app.route('/video_feed')
def video_feed():
    return Response(generate_frames(),
                    mimetype='multipart/x-mixed-replace; boundary=frame')

@app.route('/')
def index():
    # 提供一个非常简单的网页来承载视频流，你可以直接使用手机或电脑的浏览器访问
    html = """
    <html>
      <head>
        <title>TencentBot-vmi Live Monitor</title>
        <style>
            body { background-color: #1a1a1a; color: white; text-align: center; margin: 0; padding: 0; }
            h2 { margin-top: 20px; font-family: sans-serif; }
            img { max-width: 100%; height: auto; border: 2px solid #444; margin-top: 10px; }
        </style>
      </head>
      <body>
        <h2>TencentBot-vmi 高帧率监控流 (DXGI)</h2>
        <!-- 这里的 /video_feed 会不断加载上面的 MJPEG 流 -->
        <img src="/video_feed" width="1280" height="720">
      </body>
    </html>
    """
    return render_template_string(html)

if __name__ == '__main__':
    print("========== TencentBot-vmi 极致流畅监控启动 ==========")
    print("正在监听 0.0.0.0:5000 ...")
    print("请在物理机的浏览器中打开: http://<虚拟机的IP地址>:5000")
    print("==================================================")
    # 监听在所有网卡上，端口 5000
    app.run(host='0.0.0.0', port=5000, threaded=True)
