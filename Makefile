# libonnxruntime 1.23.2

CC = gcc
CXX = g++

CFLAGS = -O2 -Wall -I./include
CXXFLAGS = $(CFLAGS) -fPIC

# Sherpa-ONNX / ONNX Runtime / OpenSL ES and OpenCode HTTP client dependencies.
LDFLAGS = -L./lib \
          -Wl,--start-group \
          -lsherpa-onnx-c-api \
          -lonnxruntime \
          -lOpenSLES \
          -lcurl \
          -lcjson \
          -lm \
          -lpthread \
          -llog \
          -Wl,--end-group \
          -lc++_shared \
          -landroid

TARGET = termux_speaker
OBJS = termux_speaker.o opencode_client.o

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) -o $@ $^ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

zipvoice_test: zipvoice_test.c
	$(CXX) $(CFLAGS) -o $@ $< -L./lib \
		-Wl,--start-group \
		-lsherpa-onnx-c-api -lonnxruntime -lm -lpthread -llog \
		-Wl,--end-group -lc++_shared -landroid

tts_server: tts_server.c
	$(CC) $(CFLAGS) -o $@ $< -L./lib \
		-lsherpa-onnx-c-api -lonnxruntime -lOpenSLES \
		-lm -lpthread -lcjson

clean:
	rm -f $(OBJS) $(TARGET) tts_server zipvoice_test

run: $(TARGET)
	@echo "运行程序..."
	./$(TARGET)

.PHONY: all clean run
