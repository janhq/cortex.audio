#include "audio_engine.h"

#include <chrono>
#include <filesystem>
#include "json/writer.h"
#include "trantor/utils/Logger.h"
#include "utils.h"

namespace {
constexpr const int k200OK = 200;
constexpr const int k400BadRequest = 400;
constexpr const int k409Conflict = 409;
constexpr const int k500InternalServerError = 500;

constexpr const auto kTypeF16 = "f16";
constexpr const auto kType_Q8_0 = "q8_0";
constexpr const auto kType_Q4_0 = "q4_0";

bool IsValidCacheType(const std::string& c) {
  if (c != kTypeF16 && c != kType_Q8_0 && c != kType_Q4_0) {
    return false;
  }
  return true;
}

Json::Value CreateEmbeddingPayload(const std::vector<float>& embedding,
                                   int prompt_tokens) {
  Json::Value dataItem;

  dataItem["object"] = "embedding";

  Json::Value embeddingArray(Json::arrayValue);
  for (const auto& value : embedding) {
    embeddingArray.append(value);
  }
  dataItem["embedding"] = embeddingArray;
  dataItem["index"] = 0;

  return dataItem;
}

Json::Value CreateFullReturnJson(const std::string& id,
                                 const std::string& model,
                                 const std::string& content,
                                 const std::string& system_fingerprint,
                                 int prompt_tokens, int completion_tokens,
                                 Json::Value finish_reason = Json::Value()) {
  Json::Value root;

  root["id"] = id;
  root["model"] = model;
  root["created"] = static_cast<int>(std::time(nullptr));
  root["object"] = "chat.completion";
  root["system_fingerprint"] = system_fingerprint;

  Json::Value choicesArray(Json::arrayValue);
  Json::Value choice;

  choice["index"] = 0;
  Json::Value message;
  message["role"] = "assistant";
  message["content"] = content;
  choice["message"] = message;
  choice["finish_reason"] = finish_reason;

  choicesArray.append(choice);
  root["choices"] = choicesArray;

  Json::Value usage;
  usage["prompt_tokens"] = prompt_tokens;
  usage["completion_tokens"] = completion_tokens;
  usage["total_tokens"] = prompt_tokens + completion_tokens;
  root["usage"] = usage;

  return root;
}

std::string CreateReturnJson(const std::string& id, const std::string& model,
                             const std::string& content,
                             Json::Value finish_reason = Json::Value()) {
  Json::Value root;

  root["id"] = id;
  root["model"] = model;
  root["created"] = static_cast<int>(std::time(nullptr));
  root["object"] = "chat.completion.chunk";

  Json::Value choicesArray(Json::arrayValue);
  Json::Value choice;

  choice["index"] = 0;
  Json::Value delta;
  delta["content"] = content;
  choice["delta"] = delta;
  choice["finish_reason"] = finish_reason;

  choicesArray.append(choice);
  root["choices"] = choicesArray;

  Json::StreamWriterBuilder writer;
  writer["indentation"] = "";  // This sets the indentation to an empty string,
                               // producing compact output.
  return Json::writeString(writer, root);
}
}  // namespace

AudioEngine::AudioEngine() {
  // log_disable();
}

AudioEngine::~AudioEngine() {}

void AudioEngine::CreateTranscription(
    std::shared_ptr<Json::Value> json_body,
    std::function<void(Json::Value&&, Json::Value&&)>&& callback) {
  // Check if model is loaded
  if (CheckModelLoaded(callback, utils::GetModelId(*json_body))) {
    // Model is loaded
    return HandleTranscriptionImpl(json_body, std::move(callback),
                                   /*translate*/ false);
  }
}

void AudioEngine::CreateTranslation(
    std::shared_ptr<Json::Value> json_body,
    std::function<void(Json::Value&&, Json::Value&&)>&& callback) {
  // Check if model is loaded
  if (CheckModelLoaded(callback, utils::GetModelId(*json_body))) {
    return HandleTranscriptionImpl(json_body, std::move(callback),
                                   /*translate*/ true);
  }
}

void AudioEngine::LoadModel(
    std::shared_ptr<Json::Value> json_body,
    std::function<void(Json::Value&&, Json::Value&&)>&& callback) {
  if (std::exchange(print_version_, false)) {
#if defined(CORTEXLLAMA_VERSION)
    LOG_INFO << "cortex.llamacpp version: " << CORTEXLLAMA_VERSION;
#else
    LOG_INFO << "cortex.llamacpp version: default_version";
#endif
  }
  auto model_id = utils::GetModelId(*json_body);
  if (model_id.empty()) {
    LOG_INFO << "Model id is empty in request";
    Json::Value jsonResp;
    jsonResp["message"] = "No model id found in request body";
    Json::Value status;
    status["is_done"] = false;
    status["has_error"] = true;
    status["is_stream"] = false;
    status["status_code"] = k400BadRequest;
    callback(std::move(status), std::move(jsonResp));
    return;
  }

  if (auto si = server_map_.find(model_id);
      si != server_map_.end() && si->second.model_loaded) {
    LOG_INFO << "Model already loaded";
    Json::Value jsonResp;
    jsonResp["message"] = "Model already loaded";
    Json::Value status;
    status["is_done"] = true;
    status["has_error"] = false;
    status["is_stream"] = false;
    status["status_code"] = k409Conflict;
    callback(std::move(status), std::move(jsonResp));
    return;
  }

  if (!LoadModelImpl(json_body)) {
    // Error occurred during model loading
    Json::Value jsonResp;
    jsonResp["message"] = "Failed to load model";
    Json::Value status;
    status["is_done"] = false;
    status["has_error"] = true;
    status["is_stream"] = false;
    status["status_code"] = k500InternalServerError;
    callback(std::move(status), std::move(jsonResp));
  } else {
    // Model loaded successfully
    Json::Value jsonResp;
    jsonResp["message"] = "Model loaded successfully";
    Json::Value status;
    status["is_done"] = true;
    status["has_error"] = false;
    status["is_stream"] = false;
    status["status_code"] = k200OK;
    callback(std::move(status), std::move(jsonResp));
    LOG_INFO << "Model loaded successfully: " << model_id;
  }
}

void AudioEngine::UnloadModel(
    std::shared_ptr<Json::Value> json_body,
    std::function<void(Json::Value&&, Json::Value&&)>&& callback) {
  auto model_id = utils::GetModelId(*json_body);
  if (CheckModelLoaded(callback, model_id)) {
    server_map_.erase(model_id);
    server_map_[model_id].model_loaded = false;
    LOG_INFO << "Model unloaded successfully";
  }
}

void AudioEngine::GetModelStatus(
    std::shared_ptr<Json::Value> json_body,
    std::function<void(Json::Value&&, Json::Value&&)>&& callback) {

  auto model_id = utils::GetModelId(*json_body);
  if (auto is_loaded = CheckModelLoaded(callback, model_id); is_loaded) {
    // CheckModelLoaded gurantees that model_id exists in server_ctx_map;
    auto si = server_map_.find(model_id);
    Json::Value jsonResp;
    jsonResp["model_loaded"] = is_loaded;
    jsonResp["model_data"] = "";
    Json::Value status;
    status["is_done"] = true;
    status["has_error"] = false;
    status["is_stream"] = false;
    status["status_code"] = k200OK;
    callback(std::move(status), std::move(jsonResp));
    LOG_INFO << "Model status responded";
  }
}

void AudioEngine::GetModels(
    std::shared_ptr<Json::Value> json_body,
    std::function<void(Json::Value&&, Json::Value&&)>&& callback) {
  Json::Value json_resp;
  Json::Value model_array(Json::arrayValue);
  for (const auto& [m, s] : server_map_) {
    if (s.model_loaded) {
      Json::Value val;
      val["id"] = m;
      val["engine"] = "cortex.llamacpp";
      // val["start_time"] = s.start_time;
      val["vram"] = "-";
      val["ram"] = "-";
      val["object"] = "model";
      model_array.append(val);
    }
  }

  json_resp["object"] = "list";
  json_resp["data"] = model_array;

  Json::Value status;
  status["is_done"] = true;
  status["has_error"] = false;
  status["is_stream"] = false;
  status["status_code"] = k200OK;
  callback(std::move(status), std::move(json_resp));
  LOG_INFO << "Running models responded";
}

bool AudioEngine::LoadModelImpl(std::shared_ptr<Json::Value> json_body) {

  auto model_id = utils::GetModelId(*json_body);
  auto model_path = (*json_body)["model_path"];
  if (model_path.isNull()) {
    LOG_ERROR << "Missing model path in request";
    return false;
  } else {
    if (std::filesystem::exists(std::filesystem::path(model_path.asString()))) {
      // params.model = model_path.asString();
    } else {
      LOG_ERROR << "Could not find model in path " << model_path.asString();
      return false;
    }
  }

  server_map_[model_id].ctx.model_id = model_id;
  auto model_path_str = model_path.asString();
  auto is_success = server_map_[model_id].ctx.LoadModel(model_path_str);
  if (!is_success) {
    LOG_ERROR << "Could not load model: " << model_path.asString();
    server_map_.erase(model_id);
    return false;
  }

  // Warm up the model
  // Parse warm up audio path from request
  if (json_body->isMember("warm_up_audio_path")) {
    std::string warm_up_msg = "Warming up model " + model_id;
    LOG_INFO << warm_up_msg;
    std::string warm_up_audio_path =
        (*json_body)["warm_up_audio_path"].asString();
    // Return 400 error if warm up audio path is not found
    if (!is_file_exist(warm_up_audio_path.c_str())) {
      std::string error_msg =
          "Warm up audio " + warm_up_audio_path +
          " not found, please provide a valid path or don't specify it at all";
      LOG_INFO << error_msg;
      return false;
    } else {
      LOG_INFO << "Warming up model " << model_id << " with audio "
               << warm_up_audio_path << " ...";
      std::string warm_up_result = server_map_[model_id].ctx.Inference(
          warm_up_audio_path, "en", "", text_format, 0, false);
      LOG_INFO << "Warm up model " << model_id << " completed";
    }
  } else {
    LOG_INFO << "No warm up audio provided, skipping warm up";
  }

  server_map_[model_id].model_loaded = true;
  server_map_[model_id].start_time =
      std::chrono::system_clock::now().time_since_epoch() /
      std::chrono::milliseconds(1);

  return true;
}

void AudioEngine::HandleTranscriptionImpl(
    std::shared_ptr<Json::Value> json_body,
    std::function<void(Json::Value&&, Json::Value&&)>&& callback,
    bool translate) {
  auto model_id = utils::GetModelId(*json_body);
  auto temp_file_path = (*json_body).get("file", "").asString();
  if (temp_file_path.empty()) {
    LOG_ERROR << "audio file not found";
    return;
  }
  auto language = (*json_body).get("language", "en").asString();
  auto prompt = (*json_body).get("prompt", "").asString();
  auto response_format =
      (*json_body).get("response_format", json_format).asString();
  auto temperature = (*json_body).get("temperature", 0.0f).asFloat();

  std::string result;
  try {
    result = server_map_[model_id].ctx.Inference(temp_file_path, language,
                                                 prompt, response_format,
                                                 temperature, translate);
    auto resp_data = CreateFullReturnJson(utils::generate_random_string(20),
                                          "_", result, "_", 0, 0);
    Json::Value status;
    status["is_done"] = true;
    status["has_error"] = false;
    status["is_stream"] = false;
    status["status_code"] = k200OK;
    callback(std::move(status), std::move(resp_data));

    LOG_DEBUG << result;
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
  }
}

bool AudioEngine::CheckModelLoaded(
    std::function<void(Json::Value&&, Json::Value&&)>& callback,
    const std::string& model_id) {
  if (auto si = server_map_.find(model_id);
      si == server_map_.end() || !si->second.model_loaded) {
    LOG_WARN << "Error: model_id: " << model_id
             << ", existed: " << (si != server_map_.end())
             << ", loaded: " << false;
    Json::Value jsonResp;
    jsonResp["message"] =
        "Model has not been loaded, please load model into cortex.llamacpp";
    Json::Value status;
    status["is_done"] = false;
    status["has_error"] = true;
    status["is_stream"] = false;
    status["status_code"] = k409Conflict;
    callback(std::move(status), std::move(jsonResp));
    return false;
  }
  return true;
}

void AudioEngine::WarmUpModel(const std::string& model_id) {}

bool AudioEngine::ShouldInitBackend() const {
  return false;
}

extern "C" {
EngineI* get_engine() {
  return new AudioEngine();
}
}