#pragma once
#include "chat_completion_request.h"
#include "cortex-common/enginei.h"
#include "trantor/utils/ConcurrentTaskQueue.h"
#include "whisper_server_context.h"

#define DR_WAV_IMPLEMENTATION
#include "dr_wav.h"

class AudioEngine: public EngineI {
 public:
  AudioEngine();
  ~AudioEngine();
  // #### Interface ####

  void HandleChatCompletion(
      std::shared_ptr<Json::Value> json_body,
      std::function<void(Json::Value&&, Json::Value&&)>&& callback) final {}
  void HandleEmbedding(
      std::shared_ptr<Json::Value> json_body,
      std::function<void(Json::Value&&, Json::Value&&)>&& callback) final {}

  void CreateTranscription(
      std::shared_ptr<Json::Value> json_body,
      std::function<void(Json::Value&&, Json::Value&&)>&& callback) final;

  void CreateTranslation(
      std::shared_ptr<Json::Value> json_body,
      std::function<void(Json::Value&&, Json::Value&&)>&& callback) final;

  void LoadModel(std::shared_ptr<Json::Value> json_body,
                 std::function<void(Json::Value&&, Json::Value&&)>&& callback) final;

  void UnloadModel(
      std::shared_ptr<Json::Value> json_body,
      std::function<void(Json::Value&&, Json::Value&&)>&& callback) final;

  void GetModelStatus(
      std::shared_ptr<Json::Value> json_body,
      std::function<void(Json::Value&&, Json::Value&&)>&& callback) final;

  void GetModels(std::shared_ptr<Json::Value> json_body,
                 std::function<void(Json::Value&&, Json::Value&&)>&& callback) final;

 private:
  bool LoadModelImpl(std::shared_ptr<Json::Value> json_body);
  void HandleTranscriptionImpl(
      std::shared_ptr<Json::Value> json_body,
      std::function<void(Json::Value&&, Json::Value&&)>&& callback,
      bool translate);
  bool CheckModelLoaded(
      std::function<void(Json::Value&&, Json::Value&&)>& callback,
      const std::string& model_id);
  void WarmUpModel(const std::string& model_id);
  bool ShouldInitBackend() const;

 private:
  struct ServerInfo {
    WhisperServerContext ctx;
    std::atomic<bool> model_loaded;
    uint64_t start_time;
  };

  // key: model_id, value: ServerInfo
  std::unordered_map<std::string, ServerInfo> server_map_;

  std::atomic<int> no_of_requests_ = 0;
  std::atomic<int> no_of_chats_ = 0;

  bool print_version_ = true;
};