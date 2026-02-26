import cv2
import numpy as np
import win32gui
import win32ui
import win32con
import win32api
from flask import Flask, Response, render_template_string, request, jsonify
import time
import threading
import os
import subprocess

app = Flask(__name__)

class WebKVM:
    def __init__(self):
        self.hdesktop = win32gui.GetDesktopWindow()
        self.width = win32api.GetSystemMetrics(win32con.SM_CXSCREEN)
        self.height = win32api.GetSystemMetrics(win32con.SM_CYSCREEN)
        
        self.is_viewing = False
        self.last_frame = None
        self.target_fps = 15
        self.quality = 60
        
    def capture_once(self):
        desktop_dc = win32gui.GetWindowDC(self.hdesktop)
        img_dc = win32ui.CreateDCFromHandle(desktop_dc)
        mem_dc = img_dc.CreateCompatibleDC()
        
        screenshot = win32ui.CreateBitmap()
        screenshot.CreateCompatibleBitmap(img_dc, self.width, self.height)
        mem_dc.SelectObject(screenshot)
        
        mem_dc.BitBlt((0, 0), (self.width, self.height), img_dc, (0, 0), win32con.SRCCOPY)
        
        signed_ints_array = screenshot.GetBitmapBits(True)
        img = np.frombuffer(signed_ints_array, dtype='uint8')
        img.shape = (self.height, self.width, 4)
        
        mem_dc.DeleteDC()
        win32gui.DeleteObject(screenshot.GetHandle())
        img_dc.DeleteDC()
        win32gui.ReleaseDC(self.hdesktop, desktop_dc)
        
        return cv2.cvtColor(img, cv2.COLOR_BGRA2BGR)

    def capture_loop(self):
        while True:
            if self.is_viewing:
                try:
                    self.last_frame = self.capture_once()
                except Exception as e:
                    pass
            time.sleep(1.0 / self.target_fps)

kvm = WebKVM()
threading.Thread(target=kvm.capture_loop, daemon=True).start()

last_access_time = 0

def generate_frames():
    global last_access_time
    kvm.is_viewing = True
    print("[系统] 用户连入 Web KVM，开启推流...")
    
    while True:
        if time.time() - last_access_time > 3:
            print("[系统] 用户断开，进入休眠省电模式.")
            kvm.is_viewing = False
            break
            
        last_access_time = time.time()
        
        frame = kvm.last_frame
        if frame is None:
            time.sleep(0.1)
            continue
            
        frame_resized = cv2.resize(frame, (1280, 720), interpolation=cv2.INTER_LINEAR)
        ret, buffer = cv2.imencode('.jpg', frame_resized, [int(cv2.IMWRITE_JPEG_QUALITY), kvm.quality])
        
        if not ret:
            continue
            
        yield (b'--frame\r\n'
               b'Content-Type: image/jpeg\r\n\r\n' + buffer.tobytes() + b'\r\n')

@app.route('/video_feed')
def video_feed():
    global last_access_time
    last_access_time = time.time()
    return Response(generate_frames(), mimetype='multipart/x-mixed-replace; boundary=frame')

# --- 远程控制接口 ---

@app.route('/mouse', methods=['POST'])
def handle_mouse():
    data = request.json
    action = data.get('action')
    nx = data.get('nx', 0.5)
    ny = data.get('ny', 0.5)
    
    # 将网页前端传来的标准化坐标 (0.0~1.0) 还原为虚拟机的真实绝对物理像素坐标
    x = int(nx * kvm.width)
    y = int(ny * kvm.height)
    
    # 移动鼠标
    win32api.SetCursorPos((x, y))
    
    # 模拟按键
    if action == 'ldown':
        win32api.mouse_event(win32con.MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0)
    elif action == 'lup':
        win32api.mouse_event(win32con.MOUSEEVENTF_LEFTUP, 0, 0, 0, 0)
    elif action == 'rdown':
        win32api.mouse_event(win32con.MOUSEEVENTF_RIGHTDOWN, 0, 0, 0, 0)
    elif action == 'rup':
        win32api.mouse_event(win32con.MOUSEEVENTF_RIGHTUP, 0, 0, 0, 0)
        
    return jsonify({"status": "ok"})

# 简单键盘映射 (可根据需求扩充)
KEY_MAP = {
    'enter': win32con.VK_RETURN, 'backspace': win32con.VK_BACK,
    'tab': win32con.VK_TAB, 'escape': win32con.VK_ESCAPE,
    ' ': win32con.VK_SPACE, 'shift': win32con.VK_SHIFT,
    'control': win32con.VK_CONTROL, 'alt': win32con.VK_MENU,
    'arrowleft': win32con.VK_LEFT, 'arrowup': win32con.VK_UP,
    'arrowright': win32con.VK_RIGHT, 'arrowdown': win32con.VK_DOWN
}

@app.route('/keyboard', methods=['POST'])
def handle_key():
    data = request.json
    key = data.get('key', '').lower()
    
    vk = None
    if key in KEY_MAP:
        vk = KEY_MAP[key]
    elif len(key) == 1:
        # A-Z, 0-9 直接转换
        vk = win32api.VkKeyScan(key) & 0xFF
        
    if vk:
        win32api.keybd_event(vk, 0, 0, 0)
        time.sleep(0.01)
        win32api.keybd_event(vk, 0, win32con.KEYEVENTF_KEYUP, 0)
        return jsonify({"status": "ok", "key": key})
    
    return jsonify({"status": "error", "message": "Unknown key"})

@app.route('/cmd', methods=['POST'])
def handle_cmd():
    """ 远程下发启动程序的指令 """
    data = request.json
    command = data.get('command')
    if command == 'start_game':
        # 此处修改为你的梦幻西游绝对路径，或者 TencentBot 的启动 bat 路径
        try:
            subprocess.Popen(r"C:\Windows\notepad.exe", shell=True) # 默认弹个记事本测试
            return jsonify({"status": "记事本已启动，请在远程桌面验证"})
        except Exception as e:
            return jsonify({"status": f"启动失败：{str(e)}"})
            
    return jsonify({"status": "unknown cmd"})


@app.route('/')
def index():
    html = """
    <html>
      <head>
        <title>TencentBot 全能 Web KVM</title>
        <style>
            body { background: #111; color: #eee; font-family: sans-serif; text-align: center; margin: 0; padding: 10px;}
            #screen { 
                max-width: 95%; 
                border: 1px solid #444; 
                cursor: crosshair;
                user-select: none;
                -webkit-user-drag: none; /* 禁用原生的图片拖拽 */
            }
            .controls { margin: 10px; }
            button { background: #007bff; color: white; border: none; padding: 8px 15px; border-radius: 4px; cursor: pointer; margin: 0 5px;}
            button:hover { background: #0056b3; }
        </style>
      </head>
      <body>
        <h2>Web KVM 管理台 (免客户端控制)</h2>
        <div class="controls">
            <!-- 点这个按钮，后端会触发上面写的 Popen() 启动程序 -->
            <button onclick="sendCmd('start_game')">🚀 远程启动应用测试 (Notepad)</button>
        </div>
        
        <p style="font-size:12px; color:#888;">提示：鼠标移入下方画面即可控制，支持左/右键拖拽。键盘按键也会实时同步。</p>
        
        <!-- 视频流画面 -->
        <img id="screen" src="/video_feed" draggable="false" />
        
        <script>
            const screenImg = document.getElementById('screen');
            let isFocused = false;
            
            // 计算网页点击坐标相对于真实屏幕的比例
            function sendMouse(action, e) {
                if(!e) return;
                const rect = screenImg.getBoundingClientRect();
                const nx = (e.clientX - rect.left) / rect.width;
                const ny = (e.clientY - rect.top) / rect.height;
                
                // 如果鼠标超出了画面范围，就不发送
                if(nx < 0 || nx > 1 || ny < 0 || ny > 1) return;
                
                fetch('/mouse', {
                    method: 'POST',
                    headers: {'Content-Type': 'application/json'},
                    body: JSON.stringify({action: action, nx: nx, ny: ny})
                });
            }
            
            // 绑定鼠标事件
            screenImg.addEventListener('mousemove', (e) => sendMouse('move', e));
            screenImg.addEventListener('mousedown', (e) => {
                isFocused = true;
                if(e.button === 0) sendMouse('ldown', e);
                else if(e.button === 2) sendMouse('rdown', e);
            });
            screenImg.addEventListener('mouseup', (e) => {
                if(e.button === 0) sendMouse('lup', e);
                else if(e.button === 2) sendMouse('rup', e);
            });
            screenImg.addEventListener('contextmenu', e => e.preventDefault()); // 禁用右键菜单
            
            // 点击外部取消焦点
            document.addEventListener('click', (e) => {
                if(e.target !== screenImg) isFocused = false;
            });
            
            // 绑定键盘事件
            window.addEventListener('keydown', (e) => {
                if(!isFocused) return; // 只有焦点在画面上才拦截键盘
                e.preventDefault(); 
                fetch('/keyboard', {
                    method: 'POST',
                    headers: {'Content-Type': 'application/json'},
                    body: JSON.stringify({key: e.key})
                });
            });
            
            function sendCmd(cmd) {
                fetch('/cmd', {
                    method: 'POST',
                    headers: {'Content-Type': 'application/json'},
                    body: JSON.stringify({command: cmd})
                }).then(r=>r.json()).then(d=>alert(d.status));
            }
        </script>
      </body>
    </html>
    """
    return render_template_string(html)

if __name__ == '__main__':
    print("========== Web KVM 全能控制台启动 ==========")
    print("-> 访问地址: http://0.0.0.0:5000")
    print("-> 将此脚本加入开机自启，即可完美告别 RDP 与 SSH！")
    print("============================================")
    
    import logging
    log = logging.getLogger('werkzeug')
    log.setLevel(logging.ERROR)
    app.run(host='0.0.0.0', port=5000, threaded=True)
