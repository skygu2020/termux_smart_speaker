# ZipVoice 独立试听与性能测试

这个测试程序独立于现有 Matcha TTS 服务，不启动 HTTP 服务、不占用 8080 端口，也不会修改 Matcha 模型或配置。

## 1. 模型文件

在项目根目录准备以下文件：

- `sherpa-onnx-zipvoice-distill-int8-zh-en-emilia/` 模型目录
- `vocos_24khz.onnx`
- `lexicon-en-us.txt`（建议下载，用于改善英文发音）

下载模型：

```sh
wget https://github.com/k2-fsa/sherpa-onnx/releases/download/tts-models/sherpa-onnx-zipvoice-distill-int8-zh-en-emilia.tar.bz2
tar xf sherpa-onnx-zipvoice-distill-int8-zh-en-emilia.tar.bz2
rm sherpa-onnx-zipvoice-distill-int8-zh-en-emilia.tar.bz2

wget https://github.com/k2-fsa/sherpa-onnx/releases/download/vocoder-models/vocos_24khz.onnx
wget https://github.com/k2-fsa/sherpa-onnx/releases/download/tts-models/lexicon-en-us.txt
```

## 2. 准备参考声音

ZipVoice 是零样本声音克隆模型，**生成声音会跟随参考音频**，并不是固定的 Emilia 女声或男声。测试命令必须同时提供参考 WAV 和与它逐字对应的参考文本。

建议使用 5–10 秒、单人、干净、没有背景音乐的普通说话录音。为了评估你希望的年轻自然女声，最好使用符合这个目标的女性参考录音；使用模型包内的示例录音只适合先验证模型能否运行，不能代表女声效果。

参考 WAV 建议为单声道 PCM 16-bit。参考文本要准确对应录音中的实际内容，不要添加录音里没有的字。

## 3. 编译

```sh
make zipvoice_test
```

程序链接项目现有的 `./include` 和 `./lib`。如果编译提示 ZipVoice 配置字段不存在，说明当前本地 Sherpa-ONNX 头文件/动态库版本不包含 ZipVoice C API，需要先升级到支持 ZipVoice 的同一套头文件和库；不要替换现有 Matcha 文件。

## 4. 运行

```sh
./zipvoice_test reference.wav "参考录音对应的文字" "需要合成的测试文本" zipvoice-test.wav 2 4
```

参数依次为：

1. 参考 WAV 路径
2. 参考音频对应的文本
3. 要合成的文本（可以中英文混合）
4. 输出 WAV 路径（可选，默认 `zipvoice-test.wav`）
5. ONNX 推理线程数（可选，默认 2）
6. ZipVoice 采样步数（可选，默认 4）

输出是模型原生 24kHz WAV，不会经过现有 Matcha 的 16kHz 播放器或任何重采样，便于单独试听和评估模型本身。

## 5. 性能指标

程序会打印：

- 首次生成回调延迟
- 总合成耗时与音频时长
- 实时倍率（音频时长 ÷ 合成耗时；大于 1 表示生成速度快于实时播放）
- CPU 时间
- 进程峰值 RSS
- 回调次数、线程数和采样步数

先用默认的 2 线程、4 steps 测试。试听质量和速度后，再分别尝试 1 线程或 2 steps；改变 steps 可能影响音质，测试时请记录参数。

## 隔离说明

此测试程序只加载 ZipVoice 模型并写出 WAV 文件。它不连接 `tts_server`，不改动 `termux_speaker`，不修改 Matcha 服务的端口、模型、OpenSL ES 播放队列或 OpenCode 插件。
