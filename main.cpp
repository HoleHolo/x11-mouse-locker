#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <X11/cursorfont.h>
#include <X11/extensions/Xfixes.h>
#include <X11/Xatom.h>
#include <iostream>
#include <thread>
#include <atomic>
#include <csignal>
#include <sys/stat.h>
#include <fstream>
#include <vector>
#include <string>
#include <algorithm> // find 函数
#include <cstring>   // strrchr 函数

using namespace std;

atomic<bool> running(true);
atomic<bool> locking(false);

// 配置文件路径
const string CONFIG_FILE = "~/.config/windowlocker.conf";

// 特殊进程列表
vector<string> specialProcesses;

// 加载配置文件
void loadConfig()
{
    string expandedPath = CONFIG_FILE;
    if (expandedPath.find("~/") == 0)
    {
        expandedPath = string(getenv("HOME")) + expandedPath.substr(1);
    }

    // 检查配置文件是否存在
    struct stat buffer;
    if (stat(expandedPath.c_str(), &buffer) != 0)
    {
        // 文件不存在，创建目录和默认配置文件
        cout << "配置文件不存在，正在创建默认配置文件: " << expandedPath << endl;

        // 创建配置目录（如果不存在）
        string configDir = expandedPath.substr(0, expandedPath.find_last_of('/'));
        if (mkdir(configDir.c_str(), 0755) != 0 && errno != EEXIST)
        {
            cerr << "无法创建配置目录: " << configDir << endl;
            return;
        }

        // 创建并写入默认配置文件
        ofstream configFile(expandedPath);
        if (!configFile.is_open())
        {
            cerr << "无法创建配置文件: " << expandedPath << endl;
            return;
        }

        // 写入默认配置内容
        configFile << "# 窗口锁定器配置文件\n";
        configFile << "# 每行一个需要特殊处理的进程名（使用指针屏障方式锁定）\n";

        configFile.close();
        cout << "已创建默认配置文件，请编辑后重新启动程序\n";
        return;
    }

    // 正常加载配置文件
    ifstream configFile(expandedPath);
    if (!configFile.is_open())
    {
        cerr << "无法打开配置文件: " << expandedPath << endl;
        return;
    }

    string line;
    while (getline(configFile, line))
    {
        // 跳过注释和空行
        if (line.empty() || line[0] == '#')
            continue;

        // 去除前后空白
        line.erase(0, line.find_first_not_of(" \t"));
        line.erase(line.find_last_not_of(" \t") + 1);

        if (!line.empty())
        {
            specialProcesses.push_back(line);
        }
    }

    configFile.close();
    cout << "已加载 " << specialProcesses.size() << " 个特殊进程配置\n";
}

// 获取窗口的进程名
string getWindowProcessName(Display *display, Window window)
{
    Atom prop = XInternAtom(display, "_NET_WM_PID", True);
    if (prop == None)
    {
        return ""; // 属性不存在
    }

    Atom type;
    int format;
    unsigned long nitems;
    unsigned long bytes_after;
    unsigned char *data = nullptr;

    // 获取窗口的 PID 属性
    if (XGetWindowProperty(display, window, prop, 0, 1, False, XA_CARDINAL,
                           &type, &format, &nitems, &bytes_after, &data) == Success)
    {
        if (data)
        {
            pid_t pid = *reinterpret_cast<pid_t *>(data);
            XFree(data);

            // 通过 PID 获取进程名
            char path[1024];
            snprintf(path, sizeof(path), "/proc/%d/cmdline", pid);

            FILE *f = fopen(path, "r");
            if (f)
            {
                char cmdline[1024] = {0};
                if (fgets(cmdline, sizeof(cmdline), f))
                {
                    fclose(f);
                    // 只取第一个参数（可执行文件名）
                    char *p = strrchr(cmdline, '/');
                    if (p)
                    {
                        return std::string(p + 1);
                    }
                    else
                    {
                        return std::string(cmdline);
                    }
                }
                fclose(f);
            }
        }
    }

    return ""; // 无法获取进程名
}

// 检查窗口是否是特殊进程
Bool isSpecialWindow(Display *display, Window window)
{
    string processName = getWindowProcessName(display, window);
    // cout << "进程名：" << processName << endl;
    if (processName.empty())
        return True;

    // 检查是否在特殊进程列表中
    return find(specialProcesses.begin(), specialProcesses.end(), processName) != specialProcesses.end();
}

// 热键监听线程函数
void hotkeyListener()
{
    Display *display = XOpenDisplay(nullptr);
    if (!display)
    {
        cerr << "无法打开 X 显示\n";
        return;
    }

    Window root = DefaultRootWindow(display);

    int keycode = XKeysymToKeycode(display, XK_L);   // L 键
    unsigned int modifiers = ControlMask | Mod1Mask; // Ctrl + Alt

    // 注册热键（处理不同的锁定状态，如 NumLock/CapsLock）
    for (int i = 0; i < 2; ++i)
    {
        for (int j = 0; j < 2; ++j)
        {
            XGrabKey(display, keycode, modifiers | (i ? Mod2Mask : 0) | (j ? LockMask : 0),
                     root, True, GrabModeAsync, GrabModeAsync);
        }
    }

    XSelectInput(display, root, KeyPressMask);

    while (running)
    {
        while (XPending(display))
        {
            XEvent event;
            XNextEvent(display, &event);
            if (event.type == KeyPress)
            {
                XKeyEvent xkey = event.xkey;
                if (xkey.keycode == keycode &&
                    (xkey.state & ControlMask) &&
                    (xkey.state & Mod1Mask))
                {
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
void signalHandler(int signum)
{
    cout << "\n接收到中断信号，准备退出...\n";
    running = false;
}

int getFocusedWindow(Display *display)
{
    Window focused;
    int revert_to;
    if (!XGetInputFocus(display, &focused, &revert_to))
    {
        return 0;
    }
    return focused;
}

int lockWindow(Display *display, Window window)
{
    int result = XGrabPointer(
        display,
        window,
        True,
        ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
        GrabModeAsync,
        GrabModeAsync,
        window, // ConfineTo: 限定指针移动区域到这个窗口
        None,
        CurrentTime);

    // 防止程序隐藏指针
    Cursor cursor = XCreateFontCursor(display, XC_arrow);
    XDefineCursor(display, window, cursor);

    // 刷新显示
    XFlush(display);

    return result == GrabSuccess ? 0 : -1;
}

int unlockWindow(Display *display, Window window)
{
    XUngrabPointer(display, CurrentTime);
    XFlush(display);
    return 0;
}

PointerBarrier top, bottom, Left, Right;
void lockWindow2(Display *display, Window window)
{
    XWindowAttributes attr;
    XGetWindowAttributes(display, window, &attr);

    // 获取窗口相对于 root window 的位置
    int winX = 0, winY = 0;
    Window child;
    XTranslateCoordinates(display, window, DefaultRootWindow(display), 0, 0, &winX, &winY, &child);

    int x = winX;
    int y = winY;
    int w = attr.width;
    int h = attr.height;

    // 设置屏障：上下左右 4 个边界
    top = XFixesCreatePointerBarrier(display, DefaultRootWindow(display),
                                     x, y, x + w, y,
                                     BarrierPositiveY, 0, NULL); // 鼠标向下阻止

    bottom = XFixesCreatePointerBarrier(display, DefaultRootWindow(display),
                                        x, y + h, x + w, y + h,
                                        BarrierNegativeY, 0, NULL); // 鼠标向上阻止

    Left = XFixesCreatePointerBarrier(display, DefaultRootWindow(display),
                                      x, y, x, y + h,
                                      BarrierPositiveX, 0, NULL); // 鼠标向右阻止

    Right = XFixesCreatePointerBarrier(display, DefaultRootWindow(display),
                                       x + w, y, x + w, y + h,
                                       BarrierNegativeX, 0, NULL); // 鼠标向左阻止

    XFlush(display);
}

void unlockWindow2(Display *display)
{
    XFixesDestroyPointerBarrier(display, top);
    XFixesDestroyPointerBarrier(display, bottom);
    XFixesDestroyPointerBarrier(display, Left);
    XFixesDestroyPointerBarrier(display, Right);
    XFlush(display);
}

int main(int argc, char *argv[])
{
    // 加载配置文件
    loadConfig();

    // 注册信号处理
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);

    // 获取 X 显示
    Display *display = XOpenDisplay(NULL);

    // 检查 X 显示
    if (!display)
    {
        cout << "无法打开 X 显示！\n";
        return 1;
    }

    thread listenerThread(hotkeyListener);
    cout << "热键监听器已启动，按 Ctrl+C 退出...\n";

    // 保存锁定状态避免重复执行
    bool locked = false;

    // 主线程可以继续做其他工作
    while (running)
    {
        this_thread::sleep_for(chrono::seconds(1));
        Window focused = getFocusedWindow(display);
        bool special = isSpecialWindow(display, focused);

        if (locking && locked == false)
        {
            if (!special)
            {
                lockWindow(display, focused);
            }
            else
            {
                lockWindow2(display, focused);
            }

            locked = !locked;
        }
        else if (!locking && locked == true)
        {
            if (!special)
            {
                unlockWindow(display, focused);
            }
            else
            {
                unlockWindow2(display);
            }
            locked = !locked;
        }
    }

    // 等待监听线程结束
    listenerThread.join();

    XCloseDisplay(display);

    cout << "程序正常退出\n";
    return 0;
}