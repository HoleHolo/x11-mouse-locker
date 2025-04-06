# 变量定义
CPP=g++
CPPFLAGS=-O2
LDFLAGS=-lX11 -lXfixes

# 目标文件
all: mouse_locker

mouse_locker: main.cpp
	$(CPP) $(CPPFLAGS) $(LDFLAGS) main.cpp -o mouse_locker

.PHONY: clean
clean:
	rm -rf mouse_locker