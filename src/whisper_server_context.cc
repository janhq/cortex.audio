#include "whisper_server_context.h"
#include <trantor/utils/Logger.h>
#include <fstream>
#include <sstream>
#include "dr_wav.h"
#include "json.hpp"

using json = nlohmann::json;

bool read_wav(const std::string& fname, std::vector<float>& pcmf32,
              std::vector<std::vector<float>>& pcmf32s, bool stereo) {
  drwav wav;
  std::vector<uint8_t> wav_data;  // used for pipe input from stdin

  if (fname == "-") {
    {
      uint8_t buf[1024];
      while (true) {
        const size_t n = fread(buf, 1, sizeof(buf), stdin);
        if (n == 0) {
          break;
        }
        wav_data.insert(wav_data.end(), buf, buf + n);
      }
    }

    if (drwav_init_memory(&wav, wav_data.data(), wav_data.size(), nullptr) ==
        false) {
      fprintf(stderr, "error: failed to open WAV file from stdin\n");
      return false;
    }

    fprintf(stderr, "%s: read %zu bytes from stdin\n", __func__,
            wav_data.size());
  } else if (drwav_init_file(&wav, fname.c_str(), nullptr) == false) {
    fprintf(stderr, "error: failed to open '%s' as WAV file\n", fname.c_str());
    return false;
  }

  if (wav.channels != 1 && wav.channels != 2) {
    fprintf(stderr, "%s: WAV file '%s' must be mono or stereo\n", __func__,
            fname.c_str());
    return false;
  }

  if (stereo && wav.channels != 2) {
    fprintf(stderr, "%s: WAV file '%s' must be stereo for diarization\n",
            __func__, fname.c_str());
    return false;
  }

  if (wav.sampleRate != COMMON_SAMPLE_RATE) {
    fprintf(stderr, "%s: WAV file '%s' must be %i kHz\n", __func__,
            fname.c_str(), COMMON_SAMPLE_RATE / 1000);
    return false;
  }

  if (wav.bitsPerSample != 16) {
    fprintf(stderr, "%s: WAV file '%s' must be 16-bit\n", __func__,
            fname.c_str());
    return false;
  }

  const uint64_t n =
      wav_data.empty()
          ? wav.totalPCMFrameCount
          : wav_data.size() / (wav.channels * wav.bitsPerSample / 8);

  std::vector<int16_t> pcm16;
  pcm16.resize(n * wav.channels);
  drwav_read_pcm_frames_s16(&wav, n, pcm16.data());
  drwav_uninit(&wav);

  // convert to mono, float
  pcmf32.resize(n);
  if (wav.channels == 1) {
    for (uint64_t i = 0; i < n; i++) {
      pcmf32[i] = float(pcm16[i]) / 32768.0f;
    }
  } else {
    for (uint64_t i = 0; i < n; i++) {
      pcmf32[i] = float(pcm16[2 * i] + pcm16[2 * i + 1]) / 65536.0f;
    }
  }

  if (stereo) {
    // convert to stereo, float
    pcmf32s.resize(2);

    pcmf32s[0].resize(n);
    pcmf32s[1].resize(n);
    for (uint64_t i = 0; i < n; i++) {
      pcmf32s[0][i] = float(pcm16[2 * i]) / 32768.0f;
      pcmf32s[1][i] = float(pcm16[2 * i + 1]) / 32768.0f;
    }
  }

  return true;
}

std::string output_str(struct whisper_context* ctx,
                       const WhisperParams& params,
                       std::vector<std::vector<float>> pcmf32s) {
  std::stringstream result;
  const int n_segments = whisper_full_n_segments(ctx);
  for (int i = 0; i < n_segments; ++i) {
    const char* text = whisper_full_get_segment_text(ctx, i);
    std::string speaker = "";

    if (params.diarize && pcmf32s.size() == 2) {
      const int64_t t0 = whisper_full_get_segment_t0(ctx, i);
      const int64_t t1 = whisper_full_get_segment_t1(ctx, i);
      speaker = estimate_diarization_speaker(pcmf32s, t0, t1);
    }

    result << speaker << text << "\n";
  }
  return result.str();
}

std::string estimate_diarization_speaker(
    std::vector<std::vector<float>> pcmf32s, int64_t t0, int64_t t1,
    bool id_only) {
  std::string speaker = "";
  const int64_t n_samples = pcmf32s[0].size();

  const int64_t is0 = timestamp_to_sample(t0, n_samples);
  const int64_t is1 = timestamp_to_sample(t1, n_samples);

  double energy0 = 0.0f;
  double energy1 = 0.0f;

  for (int64_t j = is0; j < is1; j++) {
    energy0 += fabs(pcmf32s[0][j]);
    energy1 += fabs(pcmf32s[1][j]);
  }

  if (energy0 > 1.1 * energy1) {
    speaker = "0";
  } else if (energy1 > 1.1 * energy0) {
    speaker = "1";
  } else {
    speaker = "?";
  }

  // printf("is0 = %lld, is1 = %lld, energy0 = %f, energy1 = %f, speaker =
  // %s\n", is0, is1, energy0, energy1, speaker.c_str());

  if (!id_only) {
    speaker.insert(0, "(speaker ");
    speaker.append(")");
  }

  return speaker;
}

//  500 -> 00:05.000
// 6000 -> 01:00.000
std::string to_timestamp(int64_t t, bool comma) {
  int64_t msec = t * 10;
  int64_t hr = msec / (1000 * 60 * 60);
  msec = msec - hr * (1000 * 60 * 60);
  int64_t min = msec / (1000 * 60);
  msec = msec - min * (1000 * 60);
  int64_t sec = msec / 1000;
  msec = msec - sec * 1000;

  char buf[32];
  snprintf(buf, sizeof(buf), "%02d:%02d:%02d%s%03d", (int)hr, (int)min,
           (int)sec, comma ? "," : ".", (int)msec);

  return std::string(buf);
}

int timestamp_to_sample(int64_t t, int n_samples) {
  return (std::max)(0, (std::min)((int)n_samples - 1,
                                  (int)((t * WHISPER_SAMPLE_RATE) / 100)));
}

bool is_file_exist(const char* fileName) {
  std::ifstream infile(fileName);
  return infile.good();
}

void whisper_print_usage(int /*argc*/, char** argv,
                         const WhisperParams& params) {
  fprintf(stderr, "\n");
  fprintf(stderr, "usage: %s [options] \n", argv[0]);
  fprintf(stderr, "\n");
  fprintf(stderr, "options:\n");
  fprintf(stderr,
          "  -h,        --help              [default] show this help "
          "message and exit\n");
  fprintf(stderr,
          "  -t N,      --threads N         [%-7d] number of threads to use "
          "during computation\n",
          params.n_threads);
  fprintf(stderr,
          "  -p N,      --processors N      [%-7d] number of processors to use "
          "during computation\n",
          params.n_processors);
  fprintf(
      stderr,
      "  -ot N,     --offset-t N        [%-7d] time offset in milliseconds\n",
      params.offset_t_ms);
  fprintf(stderr,
          "  -on N,     --offset-n N        [%-7d] segment index offset\n",
          params.offset_n);
  fprintf(stderr,
          "  -d  N,     --duration N        [%-7d] duration of audio to "
          "process in milliseconds\n",
          params.duration_ms);
  fprintf(stderr,
          "  -mc N,     --max-context N     [%-7d] maximum number of text "
          "context tokens to store\n",
          params.max_context);
  fprintf(stderr,
          "  -ml N,     --max-len N         [%-7d] maximum segment length in "
          "characters\n",
          params.max_len);
  fprintf(stderr,
          "  -sow,      --split-on-word     [%-7s] split on word rather than "
          "on token\n",
          params.split_on_word ? "true" : "false");
  fprintf(stderr,
          "  -bo N,     --best-of N         [%-7d] number of best candidates "
          "to keep\n",
          params.best_of);
  fprintf(stderr,
          "  -bs N,     --beam-size N       [%-7d] beam size for beam search\n",
          params.beam_size);
  fprintf(stderr,
          "  -wt N,     --word-thold N      [%-7.2f] word timestamp "
          "probability threshold\n",
          params.word_thold);
  fprintf(stderr,
          "  -et N,     --entropy-thold N   [%-7.2f] entropy threshold for "
          "decoder fail\n",
          params.entropy_thold);
  fprintf(stderr,
          "  -lpt N,    --logprob-thold N   [%-7.2f] log probability threshold "
          "for decoder fail\n",
          params.logprob_thold);
  // fprintf(stderr, "  -su,       --speed-up          [%-7s] speed up audio by
  // x2 (reduced accuracy)\n",        params.speed_up ? "true" : "false");
  fprintf(stderr,
          "  -debug,    --debug-mode        [%-7s] enable debug mode (eg. dump "
          "log_mel)\n",
          params.debug_mode ? "true" : "false");
  fprintf(stderr,
          "  -tr,       --translate         [%-7s] translate from source "
          "language to english\n",
          params.translate ? "true" : "false");
  fprintf(stderr,
          "  -di,       --diarize           [%-7s] stereo audio diarization\n",
          params.diarize ? "true" : "false");
  fprintf(stderr,
          "  -tdrz,     --tinydiarize       [%-7s] enable tinydiarize "
          "(requires a tdrz model)\n",
          params.tinydiarize ? "true" : "false");
  fprintf(stderr,
          "  -nf,       --no-fallback       [%-7s] do not use temperature "
          "fallback while decoding\n",
          params.no_fallback ? "true" : "false");
  fprintf(stderr,
          "  -ps,       --print-special     [%-7s] print special tokens\n",
          params.print_special ? "true" : "false");
  fprintf(stderr, "  -pc,       --print-colors      [%-7s] print colors\n",
          params.print_colors ? "true" : "false");
  fprintf(stderr,
          "  -pr,       --print-realtime    [%-7s] print output in realtime\n",
          params.print_realtime ? "true" : "false");
  fprintf(stderr, "  -pp,       --print-progress    [%-7s] print progress\n",
          params.print_progress ? "true" : "false");
  fprintf(stderr,
          "  -nt,       --no-timestamps     [%-7s] do not print timestamps\n",
          params.no_timestamps ? "true" : "false");
  fprintf(stderr,
          "  -l LANG,   --language LANG     [%-7s] spoken language ('auto' for "
          "auto-detect)\n",
          params.language.c_str());
  fprintf(stderr,
          "  -dl,       --detect-language   [%-7s] exit after automatically "
          "detecting language\n",
          params.detect_language ? "true" : "false");
  fprintf(stderr, "             --prompt PROMPT     [%-7s] initial prompt\n",
          params.prompt.c_str());
  fprintf(stderr, "  -m FNAME,  --model FNAME       [%-7s] model path\n",
          params.model.c_str());
  fprintf(stderr,
          "  -oved D,   --ov-e-device DNAME [%-7s] the OpenVINO device used "
          "for encode inference\n",
          params.openvino_encode_device.c_str());
  fprintf(stderr,
          "  --convert,                     [%-7s] Convert audio to WAV, "
          "requires ffmpeg on the server",
          params.ffmpeg_converter ? "true" : "false");
  fprintf(stderr, "\n");
}

bool WhisperParams_parse(int argc, char** argv, WhisperParams& params) {
  for (int i = 1; i < argc; i++) {
    std::string arg = argv[i];

    if (arg == "-h" || arg == "--help") {
      whisper_print_usage(argc, argv, params);
      exit(0);
    } else if (arg == "-t" || arg == "--threads") {
      params.n_threads = std::stoi(argv[++i]);
    } else if (arg == "-p" || arg == "--processors") {
      params.n_processors = std::stoi(argv[++i]);
    } else if (arg == "-ot" || arg == "--offset-t") {
      params.offset_t_ms = std::stoi(argv[++i]);
    } else if (arg == "-on" || arg == "--offset-n") {
      params.offset_n = std::stoi(argv[++i]);
    } else if (arg == "-d" || arg == "--duration") {
      params.duration_ms = std::stoi(argv[++i]);
    } else if (arg == "-mc" || arg == "--max-context") {
      params.max_context = std::stoi(argv[++i]);
    } else if (arg == "-ml" || arg == "--max-len") {
      params.max_len = std::stoi(argv[++i]);
    } else if (arg == "-bo" || arg == "--best-of") {
      params.best_of = std::stoi(argv[++i]);
    } else if (arg == "-bs" || arg == "--beam-size") {
      params.beam_size = std::stoi(argv[++i]);
    } else if (arg == "-wt" || arg == "--word-thold") {
      params.word_thold = std::stof(argv[++i]);
    } else if (arg == "-et" || arg == "--entropy-thold") {
      params.entropy_thold = std::stof(argv[++i]);
    } else if (arg == "-lpt" || arg == "--logprob-thold") {
      params.logprob_thold = std::stof(argv[++i]);
    }
    // else if (arg == "-su"   || arg == "--speed-up")        { params.speed_up
    // = true; }
    else if (arg == "-debug" || arg == "--debug-mode") {
      params.debug_mode = true;
    } else if (arg == "-tr" || arg == "--translate") {
      params.translate = true;
    } else if (arg == "-di" || arg == "--diarize") {
      params.diarize = true;
    } else if (arg == "-tdrz" || arg == "--tinydiarize") {
      params.tinydiarize = true;
    } else if (arg == "-sow" || arg == "--split-on-word") {
      params.split_on_word = true;
    } else if (arg == "-nf" || arg == "--no-fallback") {
      params.no_fallback = true;
    } else if (arg == "-fp" || arg == "--font-path") {
      params.font_path = argv[++i];
    } else if (arg == "-ps" || arg == "--print-special") {
      params.print_special = true;
    } else if (arg == "-pc" || arg == "--print-colors") {
      params.print_colors = true;
    } else if (arg == "-pr" || arg == "--print-realtime") {
      params.print_realtime = true;
    } else if (arg == "-pp" || arg == "--print-progress") {
      params.print_progress = true;
    } else if (arg == "-nt" || arg == "--no-timestamps") {
      params.no_timestamps = true;
    } else if (arg == "-l" || arg == "--language") {
      params.language = argv[++i];
    } else if (arg == "-dl" || arg == "--detect-language") {
      params.detect_language = true;
    } else if (arg == "--prompt") {
      params.prompt = argv[++i];
    } else if (arg == "-m" || arg == "--model") {
      params.model = argv[++i];
    } else if (arg == "-oved" || arg == "--ov-e-device") {
      params.openvino_encode_device = argv[++i];
    } else if (arg == "-ng" || arg == "--no-gpu") {
      params.use_gpu = false;
    } else if (arg == "--convert") {
      params.ffmpeg_converter = true;
    } else {
      fprintf(stderr, "error: unknown argument: %s\n", arg.c_str());
      whisper_print_usage(argc, argv, params);
      exit(0);
    }
  }

  return true;
}

void check_ffmpeg_availibility() {
  int result = system("ffmpeg -version");

  if (result == 0) {
    std::cout << "ffmpeg is available." << std::endl;
  } else {
    // ffmpeg is not available
    std::cout << "ffmpeg is not found. Please ensure that ffmpeg is installed ";
    std::cout << "and that its executable is included in your system's PATH. ";
    exit(0);
  }
}

bool convert_to_wav(const std::string& temp_filename, std::string& error_resp) {
  std::ostringstream cmd_stream;
  std::string converted_filename_temp = temp_filename + "_temp.wav";
  cmd_stream << "ffmpeg -i \"" << temp_filename
             << "\" -ar 16000 -ac 1 -c:a pcm_s16le \""
             << converted_filename_temp << "\" 2>&1";
  std::string cmd = cmd_stream.str();

  int status = std::system(cmd.c_str());
  if (status != 0) {
    error_resp = "{\"error\":\"FFmpeg conversion failed.\"}";
    return false;
  }

  // Remove the original file
  if (remove(temp_filename.c_str()) != 0) {
    error_resp = "{\"error\":\"Failed to remove the original file.\"}";
    return false;
  }

  // Rename the temporary file to match the original filename
  if (rename(converted_filename_temp.c_str(), temp_filename.c_str()) != 0) {
    error_resp = "{\"error\":\"Failed to rename the temporary file.\"}";
    return false;
  }
  return true;
}

void whisper_print_progress_callback(struct whisper_context* /*ctx*/,
                                     struct whisper_state* /*state*/,
                                     int progress, void* user_data) {
  int progress_step =
      ((WhisperPrintUserData*)user_data)->params->progress_step;
  int* progress_prev = &(((WhisperPrintUserData*)user_data)->progress_prev);
  if (progress >= *progress_prev + progress_step) {
    *progress_prev += progress_step;
    fprintf(stderr, "%s: progress = %3d%%\n", __func__, progress);
  }
}

void whisper_print_segment_callback(struct whisper_context* ctx,
                                    struct whisper_state* /*state*/, int n_new,
                                    void* user_data) {
  const auto& params = *((WhisperPrintUserData*)user_data)->params;
  const auto& pcmf32s = *((WhisperPrintUserData*)user_data)->pcmf32s;

  const int n_segments = whisper_full_n_segments(ctx);

  std::string speaker = "";

  int64_t t0 = 0;
  int64_t t1 = 0;

  // print the last n_new segments
  const int s0 = n_segments - n_new;

  if (s0 == 0) {
    printf("\n");
  }

  for (int i = s0; i < n_segments; i++) {
    if (!params.no_timestamps || params.diarize) {
      t0 = whisper_full_get_segment_t0(ctx, i);
      t1 = whisper_full_get_segment_t1(ctx, i);
    }

    if (!params.no_timestamps) {
      printf("[%s --> %s]  ", to_timestamp(t0).c_str(),
             to_timestamp(t1).c_str());
    }

    if (params.diarize && pcmf32s.size() == 2) {
      speaker = estimate_diarization_speaker(pcmf32s, t0, t1);
    }

    if (params.print_colors) {
      for (int j = 0; j < whisper_full_n_tokens(ctx, i); ++j) {
        if (params.print_special == false) {
          const whisper_token id = whisper_full_get_token_id(ctx, i, j);
          if (id >= whisper_token_eot(ctx)) {
            continue;
          }
        }

        const char* text = whisper_full_get_token_text(ctx, i, j);
        const float p = whisper_full_get_token_p(ctx, i, j);

        const int col = (std::max)(
            0, (std::min)((int)k_colors.size() - 1,
                          (int)((std::pow)(p, 3) * float(k_colors.size()))));

        printf("%s%s%s%s", speaker.c_str(), k_colors[col].c_str(), text,
               "\033[0m");
      }
    } else {
      const char* text = whisper_full_get_segment_text(ctx, i);

      printf("%s%s", speaker.c_str(), text);
    }

    if (params.tinydiarize) {
      if (whisper_full_get_segment_speaker_turn_next(ctx, i)) {
        printf("%s", params.tdrz_speaker_turn.c_str());
      }
    }

    // with timestamps or speakers: each segment on new line
    if (!params.no_timestamps || params.diarize) {
      printf("\n");
    }
    fflush(stdout);
  }
}

WhisperServerContext::~WhisperServerContext() {
  if (ctx) {
    whisper_print_timings(ctx);
    whisper_free(ctx);
    ctx = nullptr;
  }
}

bool WhisperServerContext::LoadModel(std::string& model_path) {
  whisper_mutex.lock();

  // clean up
  whisper_free(ctx);

  // whisper init
  ctx = whisper_init_from_file_with_params(model_path.c_str(), cparams);

  // TODO perhaps load prior model here instead of exit
  if (ctx == nullptr) {
    whisper_mutex.unlock();
    return false;
  }

  // initialize openvino encoder. this has no effect on whisper.cpp builds that
  // don't have OpenVINO configured
  whisper_ctx_init_openvino_encoder(
      ctx, nullptr, params.openvino_encode_device.c_str(), nullptr);

  // check if the model is in the file system
  whisper_mutex.unlock();
  return true;
}

std::string WhisperServerContext::Inference(
    std::string& input_file_path, std::string language, std::string prompt,
    std::string response_format, float temperature, bool translate) {
  // acquire whisper model mutex lock
  whisper_mutex.lock();

  // audio arrays
  std::vector<float> pcmf32;                // mono-channel F32 PCM
  std::vector<std::vector<float>> pcmf32s;  // stereo-channel F32 PCM

  // if file is not wav, convert to wav
  if (params.ffmpeg_converter) {
    std::string error_resp = "Failed to execute ffmpeg command converting " +
                             input_file_path + " to wav";
    const bool is_converted = convert_to_wav(input_file_path, error_resp);
    if (!is_converted) {
      whisper_mutex.unlock();
      LOG_ERROR << error_resp;
      throw std::runtime_error(error_resp);
    }
  }

  // read wav content into pcmf32
  if (!read_wav(input_file_path, pcmf32, pcmf32s, params.diarize)) {
    std::string error_resp = "Failed to read WAV file " + input_file_path;
    LOG_ERROR << error_resp;
    whisper_mutex.unlock();
    throw std::runtime_error(error_resp);
  }

  printf("Successfully loaded %s\n", input_file_path.c_str());

  params.translate = translate;
  params.language = language;
  params.response_format = response_format;
  if (!whisper_is_multilingual(ctx)) {
    if (params.language != "en" || params.translate) {
      params.language = "en";
      params.translate = false;
      LOG_WARN
          << "Model " << model_id
          << " is not multilingual, ignoring language and translation options";
    }
  }
  if (params.detect_language) {
    params.language = "auto";
  }

  // print some processing info
  std::string processing_info =
      "Model " + model_id + " processing " + input_file_path + " (" +
      std::to_string(pcmf32.size()) + " samples, " +
      std::to_string(float(pcmf32.size()) / WHISPER_SAMPLE_RATE) + " sec), " +
      std::to_string(params.n_threads) + " threads, " +
      std::to_string(params.n_processors) +
      " processors, lang = " + params.language +
      ", task = " + (params.translate ? "translate" : "transcribe") + ", " +
      (params.tinydiarize ? "tdrz = 1, " : "") +
      (params.no_timestamps ? "timestamps = 0" : "timestamps = 1");
  LOG_INFO << processing_info;

  // run the inference
  {
    std::string msg = "Running whisper.cpp inference of model " + model_id +
                      " on " + input_file_path;
    LOG_INFO << msg;
    whisper_full_params wparams =
        whisper_full_default_params(WHISPER_SAMPLING_GREEDY);

    wparams.strategy = params.beam_size > 1 ? WHISPER_SAMPLING_BEAM_SEARCH
                                            : WHISPER_SAMPLING_GREEDY;

    wparams.print_realtime = false;
    wparams.print_progress = params.print_progress;
    wparams.print_timestamps = !params.no_timestamps;
    wparams.print_special = params.print_special;
    wparams.translate = params.translate;
    wparams.language = params.language.c_str();
    wparams.detect_language = params.detect_language;
    wparams.n_threads = params.n_threads;
    wparams.n_max_text_ctx =
        params.max_context >= 0 ? params.max_context : wparams.n_max_text_ctx;
    wparams.offset_ms = params.offset_t_ms;
    wparams.duration_ms = params.duration_ms;

    wparams.thold_pt = params.word_thold;
    wparams.max_len = params.max_len == 0 ? 60 : params.max_len;
    wparams.split_on_word = params.split_on_word;

    // TODO(sang)
    // wparams.speed_up = params.speed_up;
    wparams.debug_mode = params.debug_mode;

    wparams.tdrz_enable = params.tinydiarize;  // [TDRZ]

    wparams.initial_prompt = prompt.c_str();

    wparams.greedy.best_of = params.best_of;
    wparams.beam_search.beam_size = params.beam_size;

    wparams.temperature = temperature;
    wparams.temperature_inc = params.temperature_inc;
    wparams.entropy_thold = params.entropy_thold;
    wparams.logprob_thold = params.logprob_thold;

    wparams.no_timestamps = params.no_timestamps;

    WhisperPrintUserData user_data = {&params, &pcmf32s, 0};

    // this callback is called on each new segment
    if (params.print_realtime) {
      wparams.new_segment_callback = whisper_print_segment_callback;
      wparams.new_segment_callback_user_data = &user_data;
    }

    if (wparams.print_progress) {
      wparams.progress_callback = whisper_print_progress_callback;
      wparams.progress_callback_user_data = &user_data;
    }

    // examples for abort mechanism
    // in examples below, we do not abort the processing, but we could if the
    // flag is set to true

    // the callback is called before every encoder run - if it returns false,
    // the processing is aborted
    {
      static bool is_aborted =
          false;  // NOTE: this should be atomic to avoid data race

      wparams.encoder_begin_callback = [](struct whisper_context* /*ctx*/,
                                          struct whisper_state* /*state*/,
                                          void* user_data) {
        bool is_aborted = *(bool*)user_data;
        return !is_aborted;
      };
      wparams.encoder_begin_callback_user_data = &is_aborted;
    }

    // the callback is called before every computation - if it returns true, the
    // computation is aborted
    {
      static bool is_aborted =
          false;  // NOTE: this should be atomic to avoid data race

      wparams.abort_callback = [](void* user_data) {
        bool is_aborted = *(bool*)user_data;
        return is_aborted;
      };
      wparams.abort_callback_user_data = &is_aborted;
    }

    if (whisper_full_parallel(ctx, wparams, pcmf32.data(), pcmf32.size(),
                              params.n_processors) != 0) {
      std::string error_resp = "Failed to process audio";
      LOG_ERROR << error_resp;
      whisper_mutex.unlock();
      throw std::runtime_error(error_resp);
    }
  }

  // return results to user
  std::string result;
  if (params.response_format == text_format) {
    result = output_str(ctx, params, pcmf32s);
  } else if (params.response_format == srt_format) {
    std::stringstream ss;
    const int n_segments = whisper_full_n_segments(ctx);
    for (int i = 0; i < n_segments; ++i) {
      const char* text = whisper_full_get_segment_text(ctx, i);
      const int64_t t0 = whisper_full_get_segment_t0(ctx, i);
      const int64_t t1 = whisper_full_get_segment_t1(ctx, i);
      std::string speaker = "";

      if (params.diarize && pcmf32s.size() == 2) {
        speaker = estimate_diarization_speaker(pcmf32s, t0, t1);
      }

      ss << i + 1 + params.offset_n << "\n";
      ss << to_timestamp(t0, true) << " --> " << to_timestamp(t1, true) << "\n";
      ss << speaker << text << "\n\n";
    }
    result = ss.str();
  } else if (params.response_format == vtt_format) {
    std::stringstream ss;

    ss << "WEBVTT\n\n";

    const int n_segments = whisper_full_n_segments(ctx);
    for (int i = 0; i < n_segments; ++i) {
      const char* text = whisper_full_get_segment_text(ctx, i);
      const int64_t t0 = whisper_full_get_segment_t0(ctx, i);
      const int64_t t1 = whisper_full_get_segment_t1(ctx, i);
      std::string speaker = "";

      if (params.diarize && pcmf32s.size() == 2) {
        speaker = estimate_diarization_speaker(pcmf32s, t0, t1, true);
        speaker.insert(0, "<v Speaker");
        speaker.append(">");
      }

      ss << to_timestamp(t0) << " --> " << to_timestamp(t1) << "\n";
      ss << speaker << text << "\n\n";
    }
    result = ss.str();
  } else if (params.response_format == vjson_format) {
    /* try to match openai/whisper's Python format */
    std::string results = output_str(ctx, params, pcmf32s);
    json jres = json{{"text", results}};
    const int n_segments = whisper_full_n_segments(ctx);
    for (int i = 0; i < n_segments; ++i) {
      json segment = json{
          {"id", i},
          {"text", whisper_full_get_segment_text(ctx, i)},
      };

      if (!params.no_timestamps) {
        segment["start"] = whisper_full_get_segment_t0(ctx, i) * 0.01;
        segment["end"] = whisper_full_get_segment_t1(ctx, i) * 0.01;
      }

      const int n_tokens = whisper_full_n_tokens(ctx, i);
      for (int j = 0; j < n_tokens; ++j) {
        whisper_token_data token = whisper_full_get_token_data(ctx, i, j);
        if (token.id >= whisper_token_eot(ctx)) {
          continue;
        }

        segment["tokens"].push_back(token.id);
        json word = json{{"word", whisper_full_get_token_text(ctx, i, j)}};
        if (!params.no_timestamps) {
          word["start"] = token.t0 * 0.01;
          word["end"] = token.t1 * 0.01;
        }
        word["probability"] = token.p;
        segment["words"].push_back(word);
      }
      jres["segments"].push_back(segment);
    }
    result = jres.dump(-1, ' ', false, json::error_handler_t::replace);
  } else {
    std::string results = output_str(ctx, params, pcmf32s);
    json jres = json{{"text", results}};
    result = jres.dump(-1, ' ', false, json::error_handler_t::replace);
  }

  // reset params to thier defaults
  params = default_params;

  // return whisper model mutex lock
  whisper_mutex.unlock();
  LOG_INFO << "Successfully processed " << input_file_path << ": " << result;

  return result;
}
