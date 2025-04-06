#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <X11/cursorfont.h>
#include <iostream> // 输入输出消息
#include <thread> // 创建线程
#include <atomic> // 线程同步原子操作
#include <csignal> // 信号处理
#include <unistd.h> // 为了 usleep

using namespace std;

atomic<bool> running(true);
atomic<bool> locking(false);

// 热键监听线程函数
void hotkeyListener() {
    Display* display = XOpenDisplay(nullptr);
    if (!display) {
        cerr << "无法打开 X 显示\n";
        return;
    }

    Window root = DefaultRootWindow(display);

    int keycode = XKeysymToKeycode(display, XK_L); // L 键
    unsigned int modifiers = ControlMask | Mod1Mask; // Ctrl + Alt

    // 注册热键（处理不同的锁定状态，如 NumLock/CapsLock）
    for (int i = 0; i < 2; ++i) {
        for (int j = 0; j < 2; ++j) {
            XGrabKey(display, keycode, modifiers | (i ? Mod2Mask : 0) | (j ? LockMask : 0),
                     root, True, GrabModeAsync, GrabModeAsync);
        }
    }

    XSelectInput(display, root, KeyPressMask);

    while (running) {
        while (XPending(display)) {
            XEvent event;
            XNextEvent(display, &event);
            if (event.type == KeyPress) {
                XKeyEvent xkey = event.xkey;
                if (xkey.keycode == keycode &&
                    (xkey.state & ControlMask) &&
                    (xkey.state & Mod1Mask)) {
                    //cout << "\n[热键触发] Ctrl+Alt+L 被按下！\n";
                    locking = !locking;
                }
            }
        }
        usleep(10000); // 睡眠 10ms，防止 CPU 占用过高
    }

    XUngrabKey(display, keycode, modifiers, root);
    XCloseDisplay(display);
}

// 信号处理函数
void signalHandler(int signum) {
    cout << "\n接收到中断信号，准备退出...\n";
    running = false;
}

int getFocusedWindow(Display *display) {
    Window focused;
    int revert_to;
    if (!XGetInputFocus(display, &focused, &revert_to)){
        return 0;
    }
    return focused;
}

int lockWindow(Display *display, Window window) {
    int result = XGrabPointer(
        display,
        window,
        True,
        ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
        GrabModeAsync,
        GrabModeAsync,
        window,   // ConfineTo: 限定指针移动区域到这个窗口
        None,
        CurrentTime
    );

    // 防止程序隐藏指针
    Cursor cursor = XCreateFontCursor(display, XC_arrow);
    XDefineCursor(display, window, cursor);

    // 刷新显示
    XFlush(display);

    return result == GrabSuccess ? 0 : -1;
}

int unlockWindow(Display *display, Window window) {
    XUngrabPointer(display, CurrentTime);
    XFlush(display);
    return 0;
}

int main(int argc, char* argv[]) {
    // 注册信号处理
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);

    // 获取 X 显示
    Display *display = XOpenDisplay(NULL);

    // 检查 X 显示
    if (!display) {
        cout << "无法打开 X 显示！\n";
        return 1;
    }

    thread listenerThread(hotkeyListener);
    cout << "热键监听器已启动，按 Ctrl+C 退出...\n";

    // 保存锁定状态避免重复执行
    bool locked = false;

    // 主线程可以继续做其他工作
    while (running) {
        this_thread::sleep_for(chrono::seconds(1));
        Window focused = getFocusedWindow(display);
        if (locking && locked==false) {
            lockWindow(display, focused);

            locked = !locked;
        } else if (!locking && locked==true)
        {
            unlockWindow(display, focused);
            locked = !locked;
        }
        
    }

    // 等待监听线程结束
    listenerThread.join();

    XCloseDisplay(display);

    cout << "程序正常退出\n";
    return 0;
}