#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/resource.h>
#include "sherpa-onnx/c-api/c-api.h"

typedef struct {
  struct timespec start;
  struct timespec first_callback;
  int callback_seen;
  unsigned long callback_count;
  unsigned long long callback_samples;
} Bench;

static double elapsed(const struct timespec *a, const struct timespec *b) {
  return (double)(b->tv_sec - a->tv_sec) +
         (double)(b->tv_nsec - a->tv_nsec) / 1000000000.0;
}

static int32_t progress_callback(const float *samples, int32_t n,
                                 float progress, void *arg) {
  Bench *bench = (Bench *)arg;
  struct timespec now;
  (void)samples;
  clock_gettime(CLOCK_MONOTONIC, &now);
  if (!bench->callback_seen) {
    bench->first_callback = now;
    bench->callback_seen = 1;
    fprintf(stderr, "[ZipVoice] first callback at %.3f s (progress %.1f%%)\n",
            elapsed(&bench->start, &now), progress * 100.0);
  }
  bench->callback_count++;
  if (n > 0) bench->callback_samples += (unsigned long long)n;
  return 1;
}

static void usage(const char *argv0) {
  fprintf(stderr,
      "Usage:\n"
      "  %s <reference.wav> <reference-text> <text-to-speak> [output.wav] [threads] [steps]\n\n"
      "Example:\n"
      "  %s voice.wav \"你好，我叫小明。\" \"今天天气不错。\" zipvoice-out.wav 2 4\n\n"
      "The reference WAV and reference text must match. ZipVoice is zero-shot\n"
      "voice cloning: its output voice follows the reference recording.\n",
      argv0, argv0);
}

int main(int argc, char **argv) {
  const char *model_dir = "./sherpa-onnx-zipvoice-distill-int8-zh-en-emilia";
  const char *vocoder = "./vocos_24khz.onnx";
  const char *english_lexicon = "./lexicon-en-us.txt";
  const char *output = argc > 4 ? argv[4] : "./zipvoice-test.wav";
  int threads = argc > 5 ? atoi(argv[5]) : 2;
  int steps = argc > 6 ? atoi(argv[6]) : 4;
  char lexicon[1024];
  SherpaOnnxOfflineTtsConfig config;
  SherpaOnnxGenerationConfig gen;
  const SherpaOnnxOfflineTts *tts = NULL;
  const SherpaOnnxWave *reference = NULL;
  const SherpaOnnxGeneratedAudio *audio = NULL;
  Bench bench;
  struct timespec end_wall, end_cpu;
  struct rusage usage_info;
  struct timespec cpu_start, cpu_end;
  double wall_sec, cpu_sec, audio_sec;
  int result = 1;

  if (argc < 4 || threads < 1 || steps < 1) {
    usage(argv[0]);
    return 2;
  }

  if (!SherpaOnnxFileExists(argv[1])) {
    fprintf(stderr, "Reference WAV not found: %s\n", argv[1]);
    return 2;
  }
  if (!SherpaOnnxFileExists("./" "sherpa-onnx-zipvoice-distill-int8-zh-en-emilia/encoder.int8.onnx") ||
      !SherpaOnnxFileExists(vocoder)) {
    fprintf(stderr, "ZipVoice model or Vocos vocoder is missing.\n");
    return 2;
  }

  snprintf(lexicon, sizeof(lexicon), "%s/lexicon.txt", model_dir);
  if (SherpaOnnxFileExists(english_lexicon)) {
    size_t used = strlen(lexicon);
    snprintf(lexicon + used, sizeof(lexicon) - used, ",%s", english_lexicon);
  } else {
    fprintf(stderr, "[WARN] lexicon-en-us.txt not found; English pronunciation may be limited.\n");
  }

  memset(&config, 0, sizeof(config));
  config.model.zipvoice.encoder = "./sherpa-onnx-zipvoice-distill-int8-zh-en-emilia/encoder.int8.onnx";
  config.model.zipvoice.decoder = "./sherpa-onnx-zipvoice-distill-int8-zh-en-emilia/decoder.int8.onnx";
  config.model.zipvoice.vocoder = vocoder;
  config.model.zipvoice.tokens = "./sherpa-onnx-zipvoice-distill-int8-zh-en-emilia/tokens.txt";
  config.model.zipvoice.lexicon = lexicon;
  config.model.num_threads = threads;
  config.model.provider = "cpu";
  config.model.debug = 0;

  fprintf(stderr, "[ZipVoice] sherpa-onnx %s\n",
          SherpaOnnxGetVersionStr());
  fprintf(stderr, "[ZipVoice] loading model (threads=%d)...\n", threads);
  tts = SherpaOnnxCreateOfflineTts(&config);
  if (!tts) {
    fprintf(stderr, "Failed to create ZipVoice TTS. Check that the installed sherpa-onnx library supports ZipVoice.\n");
    goto cleanup;
  }

  reference = SherpaOnnxReadWave(argv[1]);
  if (!reference || !reference->samples || reference->num_samples <= 0) {
    fprintf(stderr, "Failed to read reference audio: %s\n", argv[1]);
    goto cleanup;
  }
  fprintf(stderr, "[ZipVoice] reference: %d Hz, %d samples (%.2f s)\n",
          reference->sample_rate, reference->num_samples,
          (double)reference->num_samples / reference->sample_rate);

  memset(&gen, 0, sizeof(gen));
  gen.speed = 1.0f;
  gen.num_steps = steps;
  gen.reference_audio = reference->samples;
  gen.reference_audio_len = reference->num_samples;
  gen.reference_sample_rate = reference->sample_rate;
  gen.reference_text = argv[2];

  memset(&bench, 0, sizeof(bench));
  clock_gettime(CLOCK_MONOTONIC, &bench.start);
  clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &cpu_start);
  audio = SherpaOnnxOfflineTtsGenerateWithConfig(
      tts, argv[3], &gen, progress_callback, &bench);
  clock_gettime(CLOCK_MONOTONIC, &end_wall);
  clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &cpu_end);

  if (!audio || !audio->samples || audio->n <= 0 || audio->sample_rate <= 0) {
    fprintf(stderr, "ZipVoice synthesis failed.\n");
    goto cleanup;
  }

  if (SherpaOnnxWriteWave(audio->samples, audio->n, audio->sample_rate, output) == 0) {
    fprintf(stderr, "Failed to write output WAV: %s\n", output);
    goto cleanup;
  }

  wall_sec = elapsed(&bench.start, &end_wall);
  cpu_sec = elapsed(&cpu_start, &cpu_end);
  audio_sec = (double)audio->n / audio->sample_rate;
  getrusage(RUSAGE_SELF, &usage_info);

  fprintf(stderr, "\n========== ZipVoice benchmark ==========\n");
  fprintf(stderr, "Output             : %s\n", output);
  fprintf(stderr, "Sample rate        : %d Hz\n", audio->sample_rate);
  fprintf(stderr, "Audio duration     : %.2f s\n", audio_sec);
  fprintf(stderr, "Wall time          : %.2f s\n", wall_sec);
  fprintf(stderr, "First callback     : %.3f s\n",
          bench.callback_seen ? elapsed(&bench.start, &bench.first_callback) : -1.0);
  fprintf(stderr, "Real-time factor   : %.2fx (audio / wall)\n",
          wall_sec > 0.0 ? audio_sec / wall_sec : 0.0);
  fprintf(stderr, "CPU time           : %.2f s\n", cpu_sec);
  fprintf(stderr, "Peak RSS            : %ld KB\n", usage_info.ru_maxrss);
  fprintf(stderr, "Progress callbacks : %lu, callback samples: %llu\n",
          bench.callback_count, bench.callback_samples);
  fprintf(stderr, "Threads / steps    : %d / %d\n", threads, steps);
  fprintf(stderr, "========================================\n");
  result = 0;

cleanup:
  if (audio) SherpaOnnxDestroyOfflineTtsGeneratedAudio(audio);
  if (reference) SherpaOnnxFreeWave(reference);
  if (tts) SherpaOnnxDestroyOfflineTts(tts);
  return result;
}
