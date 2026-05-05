CXX      := D:/msys64/mingw64/bin/g++.exe
WINDRES  := D:/msys64/mingw64/bin/windres.exe

export TMP    := $(CURDIR)/.tmp
export TEMP   := $(CURDIR)/.tmp
export TMPDIR := $(CURDIR)/.tmp

CXXFLAGS := -std=c++17 -O2 -Wall -Wno-unused-parameter \
            -DWIN32_LEAN_AND_MEAN -DUNICODE -D_UNICODE \
            -D_WIN32_WINNT=0x0601

INCLUDES := -ID:/msys64/mingw64/include -Isrc

LIBS     := -static -static-libgcc -static-libstdc++ \
            -lopus \
            -lws2_32 -liphlpapi -lwinhttp -lole32 -loleaut32 -luuid \
            -lcomctl32 -lcomdlg32 -lgdi32 -luser32 -lwinmm -lkernel32 \
            -lavrt -lksuser \
            -municode -mwindows

SRCS := src/main.cpp \
        src/audio/wasapi.cpp \
        src/audio/codec.cpp \
        src/network/socket.cpp \
        src/network/stun.cpp \
        src/network/rendezvous.cpp \
        src/core/app.cpp \
        src/core/config.cpp \
        src/core/log.cpp \
        src/core/master.cpp \
        src/core/client.cpp

OBJS := $(SRCS:.cpp=.o)
RES  := src/fivecom.res

TARGET := fivecom-voice-chat.exe

all: $(TARGET)

$(TARGET): $(OBJS) $(RES)
	$(CXX) $(CXXFLAGS) $(OBJS) $(RES) -o $@ $(LIBS)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

%.res: %.rc
	$(WINDRES) $< -O coff -o $@

clean:
	@rm -f $(OBJS) $(RES) $(TARGET)

run: $(TARGET)
	./$(TARGET)

.PHONY: all clean run
