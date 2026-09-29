// SPDX-License-Identifier: Boost-1.0
#include "protocol.hpp"

#include <td/telegram/Client.h>
#include <td/telegram/td_api.h>
#include <td/telegram/td_api.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cctype>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <fcntl.h>
#include <map>
#include <memory>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace tg = aera::telegram;
namespace td_api = td::td_api;

namespace detail {
template <class... Fs> struct Overload;
template <class F> struct Overload<F> : F { explicit Overload(F f) : F(f) {} };
template <class F, class... Fs>
struct Overload<F, Fs...> : Overload<F>, Overload<Fs...> {
  Overload(F f, Fs... fs) : Overload<F>(f), Overload<Fs...>(fs...) {}
  using Overload<F>::operator();
  using Overload<Fs...>::operator();
};
}  // namespace detail
template <class... F> auto Overloaded(F... f) { return detail::Overload<F...>(f...); }

class Worker {
 public:
  int Run() {
    td::ClientManager::execute(td_api::make_object<td_api::setLogVerbosityLevel>(0));
    manager_ = std::make_unique<td::ClientManager>();
    client_id_ = manager_->create_client_id();
    LoadClientConfiguration();
    // Creating a ClientManager ID is lazy. Kick the client actor so TDLib
    // publishes its initial authorizationStateWaitTdlibParameters update.
    Send(td_api::make_object<td_api::getOption>("version"));
    SendState(tg::AuthState::kStarting, "Starting secure Telegram session");

    while (!stopping_) {
      PollControl();
      for (int i = 0; i < 32; ++i) {
        auto response = manager_->receive(i == 0 ? 0.025 : 0.0);
        if (!response.object) break;
        ProcessResponse(std::move(response));
      }
    }
    if (authorized_) Send(td_api::make_object<td_api::close>());
    return 0;
  }

 private:
  using Object = td_api::object_ptr<td_api::Object>;
  std::unique_ptr<td::ClientManager> manager_;
  int32_t client_id_ = 0;
  uint64_t query_id_ = 0;
  uint64_t auth_generation_ = 0;
  bool stopping_ = false;
  bool authorized_ = false;
  bool waiting_parameters_ = false;
  int32_t api_id_ = 0;
  std::string api_hash_;
  std::string database_key_;
  std::map<uint64_t, std::function<void(Object)>> handlers_;
  std::map<int64_t, std::string> users_;
  std::map<int64_t, std::string> chat_titles_;
  std::map<int64_t, std::string> chat_previews_;
  std::map<int64_t, uint32_t> chat_unread_;
  std::map<int64_t, int64_t> last_read_outbox_;
  std::map<int64_t, std::map<int64_t, int64_t>> outgoing_display_ids_;
  std::map<int64_t, std::string> user_avatars_;
  std::map<int64_t, std::string> chat_avatars_;
  std::map<int32_t, std::string> downloads_;
  struct PhotoPreviewDownload {
    int32_t attachment_file_id = 0;
    std::string target;
  };
  std::map<int32_t, PhotoPreviewDownload> photo_previews_;
  std::map<int32_t, std::string> avatar_downloads_;

  struct Attachment {
    int32_t file_id = 0;
    int32_t preview_file_id = 0;
    int64_t size = 0;
    std::string name;
    std::string preview_path;
    std::string preview_source_path;
    bool photo = false;
  };

  bool LoadClientConfigurationAt(const char *path, bool require_private) {
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return false;
    struct stat info{};
    char buffer[160]{};
    const ssize_t count = fstat(fd, &info) == 0 && S_ISREG(info.st_mode) &&
        (!require_private || !(info.st_mode & 0077)) &&
        info.st_size > 0 && info.st_size < 160
        ? read(fd, buffer, sizeof(buffer) - 1) : -1;
    close(fd);
    if (count <= 0) return false;
    std::string content(buffer, static_cast<size_t>(count));
    const auto split = content.find('\n');
    if (split == std::string::npos) return false;
    const long id = strtol(content.substr(0, split).c_str(), nullptr, 10);
    std::string hash = content.substr(split + 1);
    while (!hash.empty() && (hash.back() == '\n' || hash.back() == '\r')) hash.pop_back();
    if (id > 0 && id <= INT32_MAX && hash.size() >= 16 && hash.size() <= 64) {
      api_id_ = static_cast<int32_t>(id);
      api_hash_ = std::move(hash);
      return true;
    }
    return false;
  }

  void LoadClientConfiguration() {
    if (!LoadClientConfigurationAt("/state/client.conf", true))
      LoadClientConfigurationAt("/etc/aera-telegram/client.conf", false);
  }

  bool SaveClientConfiguration() {
    const std::string content = std::to_string(api_id_) + "\n" + api_hash_ + "\n";
    int fd = open("/state/client.conf",
                  O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) return false;
    const bool ok = fchmod(fd, 0600) == 0 &&
        write(fd, content.data(), content.size()) ==
            static_cast<ssize_t>(content.size()) && fsync(fd) == 0;
    close(fd);
    return ok;
  }

  static void Copy(char (&output)[tg::kTextBytes], const std::string &text) {
    const size_t count = std::min(text.size(), sizeof(output) - 1);
    memcpy(output, text.data(), count);
    output[count] = '\0';
  }

  bool Emit(tg::Kind kind, uint32_t value = 0, int64_t primary = 0,
            int64_t secondary = 0, const std::string &text = {}) {
    tg::Message message;
    message.kind = kind;
    message.value = value;
    message.primary = primary;
    message.secondary = secondary;
    Copy(message.text, text);
    while (true) {
      const ssize_t count = send(4, &message, sizeof(message), MSG_NOSIGNAL);
      if (count == static_cast<ssize_t>(sizeof(message))) return true;
      if (count < 0 && errno == EINTR) continue;
      stopping_ = true;
      return false;
    }
  }

  void SendState(tg::AuthState state, const std::string &detail = {}) {
    Emit(tg::Kind::kState, static_cast<uint32_t>(state), 0, 0, detail);
  }

  void Error(const std::string &message) { Emit(tg::Kind::kError, 0, 0, 0, message); }

  template <class Function>
  void Send(td_api::object_ptr<Function> function,
            std::function<void(Object)> handler = {}) {
    const uint64_t id = ++query_id_;
    if (handler) handlers_.emplace(id, std::move(handler));
    manager_->send(client_id_, id, std::move(function));
  }

  std::function<void(Object)> AuthHandler() {
    const uint64_t generation = auth_generation_;
    return [this, generation](Object object) {
      if (generation != auth_generation_) return;
      if (object && object->get_id() == td_api::error::ID) {
        const auto &error = static_cast<const td_api::error &>(*object);
        Error(error.message_);
      }
    };
  }

  static std::string Text(const td_api::message &message) {
    if (!message.content_) return "Unsupported message";
    switch (message.content_->get_id()) {
      case td_api::messageText::ID:
        return static_cast<const td_api::messageText &>(*message.content_).text_->text_;
      case td_api::messagePhoto::ID: {
        const auto &photo =
            static_cast<const td_api::messagePhoto &>(*message.content_);
        return photo.caption_ && !photo.caption_->text_.empty()
            ? photo.caption_->text_ : "Photo";
      }
      case td_api::messageVideo::ID: return "Video";
      case td_api::messageAudio::ID: return "Audio";
      case td_api::messageVoiceNote::ID: return "Voice message";
      case td_api::messageDocument::ID: return "Document";
      case td_api::messageSticker::ID: return "Sticker";
      default: return "Message";
    }
  }

  static std::string SafeName(std::string name, const char *fallback) {
    std::string output;
    output.reserve(std::min<size_t>(name.size(), 160));
    for (unsigned char character : name) {
      if (output.size() >= 160) break;
      if (character < 0x20 || character == 0x7f || character == '/' ||
          character == '\\') {
        output.push_back('_');
      } else {
        output.push_back(static_cast<char>(character));
      }
    }
    while (!output.empty() &&
           (output.back() == ' ' || output.back() == '.')) output.pop_back();
    if (output.empty() || output == "." || output == "..") output = fallback;
    return output;
  }

  static std::string SaveInlinePreview(
      int64_t message_id, const td_api::minithumbnail *thumbnail) {
    if (!thumbnail || thumbnail->data_.size() < 4 ||
        thumbnail->data_.size() > 512 * 1024) return {};
    const auto &data = thumbnail->data_;
    const bool jpeg = static_cast<unsigned char>(data[0]) == 0xff &&
                      static_cast<unsigned char>(data[1]) == 0xd8;
    const bool png = data.size() >= 8 &&
        !memcmp(data.data(), "\x89PNG\r\n\x1a\n", 8);
    if (!jpeg && !png) return {};
    if (mkdir("/state/previews", 0700) && errno != EEXIST) return {};
    const std::string path = "/state/previews/photo-" +
        std::to_string(message_id) + (png ? ".png" : ".jpg");
    const int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC |
                        O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) return {};
    size_t offset = 0;
    while (offset < data.size()) {
      const ssize_t count = write(fd, data.data() + offset,
                                  data.size() - offset);
      if (count < 0 && errno == EINTR) continue;
      if (count <= 0) break;
      offset += static_cast<size_t>(count);
    }
    const bool ok = offset == data.size() && fchmod(fd, 0600) == 0 &&
                    fsync(fd) == 0;
    close(fd);
    return ok ? path : std::string{};
  }

  static Attachment MessageAttachment(const td_api::message &message) {
    Attachment result;
    if (!message.content_) return result;
    switch (message.content_->get_id()) {
      case td_api::messageDocument::ID: {
        const auto &content =
            static_cast<const td_api::messageDocument &>(*message.content_);
        if (content.document_ && content.document_->document_) {
          result.file_id = content.document_->document_->id_;
          result.size = std::max(content.document_->document_->size_,
                                 content.document_->document_->expected_size_);
          result.name = SafeName(content.document_->file_name_, "document.bin");
        }
        break;
      }
      case td_api::messageVideo::ID: {
        const auto &content =
            static_cast<const td_api::messageVideo &>(*message.content_);
        if (content.video_ && content.video_->video_) {
          result.file_id = content.video_->video_->id_;
          result.size = std::max(content.video_->video_->size_,
                                 content.video_->video_->expected_size_);
          result.name = SafeName(content.video_->file_name_, "video.mp4");
        }
        break;
      }
      case td_api::messageAudio::ID: {
        const auto &content =
            static_cast<const td_api::messageAudio &>(*message.content_);
        if (content.audio_ && content.audio_->audio_) {
          result.file_id = content.audio_->audio_->id_;
          result.size = std::max(content.audio_->audio_->size_,
                                 content.audio_->audio_->expected_size_);
          result.name = SafeName(content.audio_->file_name_, "audio.bin");
        }
        break;
      }
      case td_api::messageVoiceNote::ID: {
        const auto &content =
            static_cast<const td_api::messageVoiceNote &>(*message.content_);
        if (content.voice_note_ && content.voice_note_->voice_) {
          result.file_id = content.voice_note_->voice_->id_;
          result.size = std::max(content.voice_note_->voice_->size_,
                                 content.voice_note_->voice_->expected_size_);
          result.name = "voice-message.ogg";
        }
        break;
      }
      case td_api::messagePhoto::ID: {
        const auto &content =
            static_cast<const td_api::messagePhoto &>(*message.content_);
        if (content.photo_) {
          const td_api::photoSize *largest = nullptr;
          const td_api::photoSize *preview = nullptr;
          for (const auto &size : content.photo_->sizes_) {
            if (!size || !size->photo_) continue;
            const int64_t area = size->width_ * int64_t(size->height_);
            if (!largest || area >
                largest->width_ * int64_t(largest->height_))
              largest = size.get();
            const int edge = std::max(size->width_, size->height_);
            if (edge <= 1600 && (!preview || area >
                preview->width_ * int64_t(preview->height_)))
              preview = size.get();
          }
          if (!preview) preview = largest;
          if (largest) {
            result.file_id = largest->photo_->id_;
            result.size = std::max(largest->photo_->size_,
                                   largest->photo_->expected_size_);
            result.name = "photo-" + std::to_string(message.id_) + ".jpg";
            result.photo = true;
            result.preview_path = SaveInlinePreview(
                message.id_, content.photo_->minithumbnail_.get());
            if (preview && preview->photo_) {
              result.preview_file_id = preview->photo_->id_;
              if (preview->photo_->local_ &&
                  preview->photo_->local_->is_downloading_completed_)
                result.preview_source_path = preview->photo_->local_->path_;
            }
          }
        }
        break;
      }
      default:
        break;
    }
    return result;
  }

  static bool WriteAll(int fd, const void *data, size_t size) {
    const auto *bytes = static_cast<const uint8_t *>(data);
    size_t offset = 0;
    while (offset < size) {
      const ssize_t count = write(fd, bytes + offset, size - offset);
      if (count < 0 && errno == EINTR) continue;
      if (count <= 0) return false;
      offset += static_cast<size_t>(count);
    }
    return true;
  }

  static std::string SaveAvatar(char kind, int64_t id,
                                const td_api::minithumbnail *thumbnail) {
    if (!thumbnail || thumbnail->data_.size() < 4 ||
        thumbnail->data_.size() > 256 * 1024) return {};
    const auto &data = thumbnail->data_;
    const bool jpeg = static_cast<unsigned char>(data[0]) == 0xff &&
                      static_cast<unsigned char>(data[1]) == 0xd8;
    const bool png = data.size() >= 8 &&
        !memcmp(data.data(), "\x89PNG\r\n\x1a\n", 8);
    if (!jpeg && !png) return {};
    if (mkdir("/state/avatars", 0700) && errno != EEXIST) return {};
    const std::string path = std::string("/state/avatars/") + kind + "-" +
        std::to_string(id) + (png ? ".png" : ".jpg");
    const int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC |
                        O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) return {};
    const bool ok = fchmod(fd, 0600) == 0 &&
                    WriteAll(fd, data.data(), data.size()) && fsync(fd) == 0;
    close(fd);
    return ok ? path : std::string{};
  }

  static std::string AvatarPath(char kind, int64_t id) {
    if (mkdir("/state/avatars", 0700) && errno != EEXIST) return {};
    return std::string("/state/avatars/") + kind + "-" +
        std::to_string(id) + ".jpg";
  }

  static std::string PhotoPreviewPath(int32_t file_id) {
    if (file_id <= 0 || (mkdir("/state/previews", 0700) && errno != EEXIST))
      return {};
    return "/state/previews/file-" + std::to_string(file_id) + ".image";
  }

  static bool CopyTrustedMedia(const std::string &source_path,
                               const std::string &target, int64_t size_limit) {
    if (source_path.empty() || target.empty()) return false;
    std::string source;
    if (!CanonicalUnder(source_path,
        {"/state/files", "/state/database", "/state/previews", "/sdcard",
         "/mnt/nas", "/usb_otg", "/external_sd"}, source)) return false;
    std::array<char, PATH_MAX> current_target{};
    if (realpath(target.c_str(), current_target.data()) &&
        source == current_target.data()) return true;
    const int input = open(source.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    struct stat info{};
    if (input < 0 || fstat(input, &info) != 0 || !S_ISREG(info.st_mode) ||
        info.st_size <= 0 || info.st_size > size_limit) {
      if (input >= 0) close(input);
      return false;
    }
    const std::string temporary = target + ".new-" +
        std::to_string(static_cast<long long>(getpid()));
    const int output = open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC |
                            O_CLOEXEC | O_NOFOLLOW, 0600);
    bool ok = output >= 0;
    std::array<uint8_t, 128 * 1024> buffer{};
    while (ok) {
      const ssize_t count = read(input, buffer.data(), buffer.size());
      if (count < 0 && errno == EINTR) continue;
      if (count < 0) { ok = false; break; }
      if (count == 0) break;
      ok = WriteAll(output, buffer.data(), static_cast<size_t>(count));
    }
    if (ok) ok = fsync(output) == 0;
    close(input);
    if (output >= 0) close(output);
    if (ok) ok = rename(temporary.c_str(), target.c_str()) == 0;
    if (!ok) unlink(temporary.c_str());
    return ok;
  }

  bool PublishAvatar(const td_api::file &file, const std::string &target) {
    if (!file.local_ || !file.local_->is_downloading_completed_ ||
        file.local_->path_.empty() || target.empty()) return false;
    const bool ok = CopyTrustedMedia(file.local_->path_, target,
                                     16 * 1024 * 1024);
    if (ok) Emit(tg::Kind::kAvatarReady, 0, 0, 0, target);
    return ok;
  }

  void QueueAvatar(const td_api::file *file, const std::string &target) {
    if (!file || file->id_ <= 0 || target.empty()) return;
    if (file->local_ && file->local_->is_downloading_completed_) {
      PublishAvatar(*file, target);
      return;
    }
    avatar_downloads_[file->id_] = target;
    Send(td_api::make_object<td_api::downloadFile>(
        file->id_, 20, 0, 0, false));
  }

  using History = std::vector<td_api::object_ptr<td_api::message>>;

  void FinishHistory(const std::shared_ptr<History> &history) {
    for (auto it = history->rbegin(); it != history->rend(); ++it)
      if (*it) EmitMessage(**it);
    Emit(tg::Kind::kMessagesDone, static_cast<uint32_t>(history->size()));
  }

  void LoadHistoryPage(int64_t chat_id, int64_t from_message_id,
                       unsigned pages_left,
                       const std::shared_ptr<History> &history) {
    Send(td_api::make_object<td_api::getChatHistory>(
             chat_id, from_message_id, 0, 100, false),
         [this, chat_id, from_message_id, pages_left, history](Object object) {
      if (!object || object->get_id() == td_api::error::ID) {
        if (history->empty()) Error("Could not load this conversation");
        else FinishHistory(history);
        return;
      }
      auto page = td::move_tl_object_as<td_api::messages>(object);
      int64_t oldest = 0;
      size_t added = 0;
      for (auto &entry : page->messages_) {
        if (!entry || entry->id_ == from_message_id) continue;
        oldest = entry->id_;
        history->push_back(std::move(entry));
        ++added;
        if (history->size() >= 200) break;
      }
      // TDLib may intentionally return fewer than requested while bringing
      // remote history into its local database. Continue from the oldest ID,
      // as required by the TDLib pagination contract.
      if (pages_left > 1 && added && oldest && history->size() < 200) {
        LoadHistoryPage(chat_id, oldest, pages_left - 1, history);
        return;
      }
      FinishHistory(history);
    });
  }

  std::string Sender(const td_api::message &message) const {
    if (!message.sender_id_) return "Unknown";
    if (message.sender_id_->get_id() == td_api::messageSenderUser::ID) {
      const int64_t id = static_cast<const td_api::messageSenderUser &>(*message.sender_id_).user_id_;
      const auto it = users_.find(id);
      return it == users_.end() ? "Telegram user" : it->second;
    }
    const int64_t id = static_cast<const td_api::messageSenderChat &>(*message.sender_id_).chat_id_;
    const auto it = chat_titles_.find(id);
    return it == chat_titles_.end() ? "Telegram chat" : it->second;
  }

  std::string SenderAvatar(const td_api::message &message) const {
    if (!message.sender_id_) return {};
    if (message.sender_id_->get_id() == td_api::messageSenderUser::ID) {
      const int64_t id = static_cast<const td_api::messageSenderUser &>(
          *message.sender_id_).user_id_;
      const auto it = user_avatars_.find(id);
      return it == user_avatars_.end() ? std::string{} : it->second;
    }
    const int64_t id = static_cast<const td_api::messageSenderChat &>(
        *message.sender_id_).chat_id_;
    const auto it = chat_avatars_.find(id);
    return it == chat_avatars_.end() ? std::string{} : it->second;
  }

  static bool CanonicalUnder(const std::string &path,
                             std::initializer_list<const char *> roots,
                             std::string &canonical) {
    if (path.find("/../") != std::string::npos ||
        path.find("//") != std::string::npos) return false;
    std::array<char, PATH_MAX> resolved{};
    if (!realpath(path.c_str(), resolved.data())) return false;
    canonical = resolved.data();
    for (const char *root : roots) {
      const size_t length = strlen(root);
      if (canonical == root ||
          (canonical.compare(0, length, root) == 0 &&
           canonical.size() > length && canonical[length] == '/')) return true;
    }
    return false;
  }

  static bool AllowedAttachmentPath(const std::string &path,
                                    std::string &canonical) {
    return CanonicalUnder(path,
        {"/sdcard", "/mnt/nas", "/usb_otg", "/external_sd"}, canonical);
  }

  static bool PhotoFile(const std::string &path) {
    std::array<unsigned char, 12> header{};
    const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return false;
    const ssize_t count = read(fd, header.data(), header.size());
    close(fd);
    if (count < 8) return false;
    const bool jpeg = header[0] == 0xff && header[1] == 0xd8;
    const bool png = !memcmp(header.data(), "\x89PNG\r\n\x1a\n", 8);
    const bool webp = count >= 12 && !memcmp(header.data(), "RIFF", 4) &&
        !memcmp(header.data() + 8, "WEBP", 4);
    return jpeg || png || webp;
  }

  static std::string UniqueDownloadPath(const std::string &requested) {
    const std::string name = SafeName(requested, "telegram-file.bin");
    const auto dot = name.find_last_of('.');
    const std::string stem = dot == std::string::npos ? name : name.substr(0, dot);
    const std::string suffix = dot == std::string::npos ? "" : name.substr(dot);
    for (unsigned index = 0; index < 1000; ++index) {
      const std::string candidate = "/downloads/" + stem +
          (index ? " (" + std::to_string(index) + ")" : "") + suffix;
      if (access(candidate.c_str(), F_OK) != 0 && errno == ENOENT)
        return candidate;
    }
    return {};
  }

  void FinishDownload(int32_t file_id, Object object) {
    const auto pending = downloads_.find(file_id);
    if (pending == downloads_.end()) return;
    const std::string name = pending->second;
    downloads_.erase(pending);
    if (!object || object->get_id() == td_api::error::ID) {
      Error("Telegram could not download " + name);
      return;
    }
    auto file = td::move_tl_object_as<td_api::file>(object);
    if (!file || !file->local_ || !file->local_->is_downloading_completed_ ||
        file->local_->path_.empty()) {
      Error("Telegram returned an incomplete download for " + name);
      return;
    }
    std::string source_path;
    if (!CanonicalUnder(file->local_->path_, {"/state/files"}, source_path)) {
      Error("Telegram returned an unsafe download path");
      return;
    }
    const std::string target_path = UniqueDownloadPath(name);
    const int input = open(source_path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    const int output = target_path.empty() ? -1 :
        open(target_path.c_str(), O_WRONLY | O_CREAT | O_EXCL |
             O_CLOEXEC | O_NOFOLLOW, 0660);
    struct stat info{};
    bool ok = input >= 0 && output >= 0 && fstat(input, &info) == 0 &&
              S_ISREG(info.st_mode) && info.st_size >= 0;
    std::array<uint8_t, 256 * 1024> buffer{};
    while (ok) {
      const ssize_t count = read(input, buffer.data(), buffer.size());
      if (count < 0 && errno == EINTR) continue;
      if (count < 0) { ok = false; break; }
      if (count == 0) break;
      ok = WriteAll(output, buffer.data(), static_cast<size_t>(count));
    }
    if (ok) ok = fsync(output) == 0;
    if (input >= 0) close(input);
    if (output >= 0) close(output);
    if (!ok) {
      if (!target_path.empty()) unlink(target_path.c_str());
      Error("AERA could not save " + name + " to internal storage");
      return;
    }
    Emit(tg::Kind::kFileReady, 100, file_id, 0,
         "/sdcard/AERA/Telegram/" + target_path.substr(11));
  }

  int64_t DisplayMessageId(int64_t chat_id, int64_t server_id) const {
    const auto chat = outgoing_display_ids_.find(chat_id);
    if (chat == outgoing_display_ids_.end()) return server_id;
    const auto message = chat->second.find(server_id);
    return message == chat->second.end() ? server_id : message->second;
  }

  void EmitMessageStatus(int64_t chat_id, int64_t server_id,
                         uint32_t status) {
    Emit(tg::Kind::kMessageStatus, status, chat_id,
         DisplayMessageId(chat_id, server_id));
  }

  void EmitMessage(const td_api::message &message) {
    Attachment attachment = MessageAttachment(message);
    std::string payload = Sender(message);
    payload.push_back('\n');
    payload += SenderAvatar(message);
    payload.push_back('\n');
    payload += Text(message);
    uint32_t message_flags = message.is_outgoing_ ? 1U : 0U;
    if (message.content_ && message.content_->get_id() == td_api::messageText::ID)
      message_flags |= 1U << 3;
    if (attachment.photo) message_flags |= 1U << 4;
    if (message.is_outgoing_) {
      outgoing_display_ids_[message.chat_id_][message.id_] = message.id_;
      if (!message.sending_state_) {
        const auto read = last_read_outbox_.find(message.chat_id_);
        const bool seen = read != last_read_outbox_.end() &&
                          message.id_ <= read->second;
        message_flags |= (seen ? 2U : 1U) << 1;
      }
    }
    Emit(tg::Kind::kMessage, message_flags,
         message.chat_id_, message.id_, payload);
    if (attachment.file_id > 0) {
      if (attachment.photo) {
        const std::string stable = PhotoPreviewPath(attachment.file_id);
        bool stable_ready = false;
        if (!attachment.preview_source_path.empty())
          stable_ready = CopyTrustedMedia(attachment.preview_source_path,
                                          stable, 64 * 1024 * 1024);
        if (stable_ready) {
          attachment.preview_path = stable;
        } else if (attachment.preview_file_id > 0) {
          // Publish the final deterministic path immediately. The UI can keep
          // its placeholder and retry this path until the asynchronous TDLib
          // download atomically fills it, even if a readiness event races the
          // initial attachment message.
          attachment.preview_path = stable;
          photo_previews_[attachment.preview_file_id] =
              {attachment.file_id, stable};
          Send(td_api::make_object<td_api::downloadFile>(
              attachment.preview_file_id, 24, 0, 0, false));
        }
      }
      Emit(tg::Kind::kAttachment, static_cast<uint32_t>(attachment.file_id),
           message.chat_id_, message.id_, attachment.name + "\n" +
           std::to_string(std::max<int64_t>(0, attachment.size)) + "\n" +
           (attachment.photo ? "photo" : "file") + "\n" +
           attachment.preview_path);
    }
  }

  void ConfigureIfReady() {
    if (!waiting_parameters_ || api_id_ <= 0 || api_hash_.size() < 16 ||
        database_key_.size() < 4) return;
    waiting_parameters_ = false;
    auto request = td_api::make_object<td_api::setTdlibParameters>();
    request->database_directory_ = "/state/database";
    request->files_directory_ = "/state/files";
    request->database_encryption_key_ = database_key_;
    request->use_file_database_ = true;
    request->use_chat_info_database_ = true;
    request->use_message_database_ = true;
    request->use_secret_chats_ = true;
    request->api_id_ = api_id_;
    request->api_hash_ = api_hash_;
    request->system_language_code_ = "en";
    request->device_model_ = "AERA Recovery";
    request->system_version_ = "Android recovery";
    request->application_version_ = "1.0.0";
    Send(std::move(request), AuthHandler());
    std::fill(database_key_.begin(), database_key_.end(), '\0');
    database_key_.clear();
    Emit(tg::Kind::kStatus, 0, 0, 0, "Opening encrypted session");
  }

  void Handle(const tg::Message &message) {
    switch (message.kind) {
      case tg::Kind::kConfigure:
        if (message.primary > 0) {
          api_id_ = static_cast<int32_t>(message.primary);
          api_hash_ = message.text;
          if (const auto split = api_hash_.find('\n'); split != std::string::npos) {
            database_key_ = api_hash_.substr(split + 1);
            api_hash_.resize(split);
          }
          if (!SaveClientConfiguration()) {
            Error("AERA could not save the private client configuration");
            break;
          }
        } else {
          database_key_ = message.text;
        }
        break;
      case tg::Kind::kPhone:
        Send(td_api::make_object<td_api::setAuthenticationPhoneNumber>(message.text, nullptr), AuthHandler());
        break;
      case tg::Kind::kCode:
        Send(td_api::make_object<td_api::checkAuthenticationCode>(message.text), AuthHandler());
        break;
      case tg::Kind::kPassword:
        Send(td_api::make_object<td_api::checkAuthenticationPassword>(message.text), AuthHandler());
        break;
      case tg::Kind::kEmail:
        Send(td_api::make_object<td_api::setAuthenticationEmailAddress>(message.text), AuthHandler());
        break;
      case tg::Kind::kEmailCode:
        Send(td_api::make_object<td_api::checkAuthenticationEmailCode>(
                 td_api::make_object<td_api::emailAddressAuthenticationCode>(message.text)), AuthHandler());
        break;
      case tg::Kind::kRegister: {
        std::string names = message.text;
        const auto newline = names.find('\n');
        Send(td_api::make_object<td_api::registerUser>(
                 names.substr(0, newline), newline == std::string::npos ? "" : names.substr(newline + 1), false),
             AuthHandler());
        break;
      }
      case tg::Kind::kLoadChats:
        Send(td_api::make_object<td_api::getChats>(nullptr, 50), [this](Object object) {
          if (!object || object->get_id() == td_api::error::ID) {
            Error("Could not load chats"); return;
          }
          auto chats = td::move_tl_object_as<td_api::chats>(object);
          for (const int64_t id : chats->chat_ids_) {
            const auto it = chat_titles_.find(id);
            const auto preview = chat_previews_.find(id);
            const auto unread = chat_unread_.find(id);
            std::string payload =
                it == chat_titles_.end() ? "Telegram chat" : it->second;
            payload += "\n";
            payload += preview == chat_previews_.end()
                ? "No recent message" : preview->second;
            payload += "\n";
            const auto avatar = chat_avatars_.find(id);
            if (avatar != chat_avatars_.end()) payload += avatar->second;
            Emit(tg::Kind::kChat,
                 unread == chat_unread_.end() ? 0 : unread->second,
                 id, 0, payload);
          }
          Emit(tg::Kind::kChatsDone);
        });
        break;
      case tg::Kind::kOpenChat: {
        const int64_t chat_id = message.primary;
        LoadHistoryPage(chat_id, 0, 4, std::make_shared<History>());
        break;
      }
      case tg::Kind::kSendText: {
        auto request = td_api::make_object<td_api::sendMessage>();
        request->chat_id_ = message.primary;
        if (message.secondary)
          request->reply_to_ =
              td_api::make_object<td_api::inputMessageReplyToMessage>(
                  message.secondary, nullptr, 0, "");
        auto content = td_api::make_object<td_api::inputMessageText>();
        content->text_ = td_api::make_object<td_api::formattedText>(message.text, td_api::array<td_api::object_ptr<td_api::textEntity>>{});
        request->input_message_content_ = std::move(content);
        Send(std::move(request), [this](Object object) {
          if (!object || object->get_id() == td_api::error::ID) Error("Message could not be sent");
        });
        break;
      }
      case tg::Kind::kSendFile: {
        std::string path;
        struct stat info{};
        if (!AllowedAttachmentPath(message.text, path) ||
            lstat(path.c_str(), &info) != 0 || !S_ISREG(info.st_mode)) {
          Error("The selected attachment is not a readable storage file");
          break;
        }
        auto request = td_api::make_object<td_api::sendMessage>();
        request->chat_id_ = message.primary;
        if (message.secondary)
          request->reply_to_ =
              td_api::make_object<td_api::inputMessageReplyToMessage>(
                  message.secondary, nullptr, 0, "");
        auto caption = td_api::make_object<td_api::formattedText>(
            "", td_api::array<td_api::object_ptr<td_api::textEntity>>{});
        if (PhotoFile(path)) {
          auto photo = td_api::make_object<td_api::inputPhoto>(
              td_api::make_object<td_api::inputFileLocal>(path), nullptr,
              nullptr, td_api::array<td_api::int32>{}, 0, 0);
          request->input_message_content_ =
              td_api::make_object<td_api::inputMessagePhoto>(
                  std::move(photo), std::move(caption), false, nullptr, false);
        } else {
          auto document = td_api::make_object<td_api::inputDocument>(
              td_api::make_object<td_api::inputFileLocal>(path), nullptr,
              false);
          request->input_message_content_ =
              td_api::make_object<td_api::inputMessageDocument>(
                  std::move(document), std::move(caption));
        }
        Send(std::move(request), [this](Object object) {
          if (!object || object->get_id() == td_api::error::ID)
            Error("Attachment could not be sent");
          else
            Emit(tg::Kind::kStatus, 0, 0, 0, "Attachment queued for upload");
        });
        break;
      }
      case tg::Kind::kEditMessage: {
        if (message.primary == 0 || message.secondary == 0 ||
            !message.text[0]) {
          Error("This message cannot be edited");
          break;
        }
        auto content = td_api::make_object<td_api::inputMessageText>();
        content->text_ = td_api::make_object<td_api::formattedText>(
            message.text,
            td_api::array<td_api::object_ptr<td_api::textEntity>>{});
        Send(td_api::make_object<td_api::editMessageText>(
            message.primary, message.secondary, nullptr, std::move(content)),
            [this](Object object) {
              if (!object || object->get_id() == td_api::error::ID)
                Error("Telegram could not edit this message");
              else
                Emit(tg::Kind::kStatus, 0, 0, 0, "Message edited");
            });
        break;
      }
      case tg::Kind::kDeleteMessage: {
        if (message.primary == 0 || message.secondary == 0) {
          Error("This message cannot be deleted");
          break;
        }
        Send(td_api::make_object<td_api::deleteMessages>(
            message.primary, td_api::array<td_api::int53>{message.secondary},
            true), [this](Object object) {
              if (!object || object->get_id() == td_api::error::ID)
                Error("Telegram could not delete this message");
            });
        break;
      }
      case tg::Kind::kDownloadFile: {
        const int64_t requested = message.primary;
        if (requested <= 0 || requested > INT32_MAX) {
          Error("This Telegram attachment cannot be downloaded");
          break;
        }
        const int32_t file_id = static_cast<int32_t>(requested);
        if (downloads_.count(file_id)) {
          Emit(tg::Kind::kStatus, 0, file_id, 0,
               "That attachment is already downloading");
          break;
        }
        downloads_[file_id] = SafeName(message.text, "telegram-file.bin");
        Emit(tg::Kind::kFileProgress, 0, file_id, 0, downloads_[file_id]);
        Send(td_api::make_object<td_api::downloadFile>(
                 file_id, 32, 0, 0, true),
             [this, file_id](Object object) {
               FinishDownload(file_id, std::move(object));
             });
        break;
      }
      case tg::Kind::kClose: stopping_ = true; break;
      default: break;
    }
    ConfigureIfReady();
  }

  void PollControl() {
    pollfd fd{4, POLLIN, 0};
    const int ready = poll(&fd, 1, 0);
    if (ready < 0 && errno == EINTR) return;
    if (ready <= 0) return;
    for (int i = 0; i < 16; ++i) {
      tg::Message message;
      const ssize_t count = recv(4, &message, sizeof(message), MSG_DONTWAIT | MSG_TRUNC);
      if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
      if (count < 0 && errno == EINTR) continue;
      if (count != static_cast<ssize_t>(sizeof(message)) || !tg::Valid(message, false)) {
        stopping_ = true; break;
      }
      Handle(message);
    }
  }

  void ProcessResponse(td::ClientManager::Response response) {
    if (response.request_id != 0) {
      const auto it = handlers_.find(response.request_id);
      if (it != handlers_.end()) {
        it->second(std::move(response.object));
        handlers_.erase(it);
      }
      return;
    }
    td_api::downcast_call(*response.object, Overloaded(
      [this](td_api::updateAuthorizationState &update) {
        ++auth_generation_;
        auto &state = *update.authorization_state_;
        td_api::downcast_call(state, Overloaded(
          [this](td_api::authorizationStateWaitTdlibParameters &) {
            waiting_parameters_ = true;
            if (api_id_ > 0 && !api_hash_.empty())
              SendState(tg::AuthState::kNeedVault,
                        "Unlock the encrypted AERA Telegram session");
            else
              SendState(tg::AuthState::kNeedConfiguration,
                        "One-time Telegram client registration");
            ConfigureIfReady();
          },
          [this](td_api::authorizationStateWaitPhoneNumber &) { SendState(tg::AuthState::kNeedPhone, "Enter your phone number with country code"); },
          [this](td_api::authorizationStateWaitCode &) { SendState(tg::AuthState::kNeedCode, "Enter the login code sent by Telegram"); },
          [this](td_api::authorizationStateWaitPassword &) { SendState(tg::AuthState::kNeedPassword, "Enter your Telegram two-step verification password"); },
          [this](td_api::authorizationStateWaitEmailAddress &) { SendState(tg::AuthState::kNeedEmail, "Enter the recovery email requested by Telegram"); },
          [this](td_api::authorizationStateWaitEmailCode &) { SendState(tg::AuthState::kNeedEmailCode, "Enter the email verification code"); },
          [this](td_api::authorizationStateWaitRegistration &) { SendState(tg::AuthState::kNeedRegistration, "Enter first and last name"); },
          [this](td_api::authorizationStateWaitOtherDeviceConfirmation &other) { SendState(tg::AuthState::kConfirmElsewhere, other.link_); },
          [this](td_api::authorizationStateReady &) {
            authorized_ = true;
            SendState(tg::AuthState::kReady, "Connected securely");
          },
          [this](td_api::authorizationStateLoggingOut &) { authorized_ = false; SendState(tg::AuthState::kClosing, "Logging out"); },
          [this](td_api::authorizationStateClosing &) { SendState(tg::AuthState::kClosing, "Closing secure session"); },
          [this](td_api::authorizationStateClosed &) { stopping_ = true; },
          [this](auto &) { Error("This Telegram authorization step is not supported yet"); }
        ));
      },
      [this](td_api::updateUser &update) {
        if (!update.user_) return;
        std::string name = update.user_->first_name_;
        if (!update.user_->last_name_.empty()) name += " " + update.user_->last_name_;
        users_[update.user_->id_] = name.empty() ? "Telegram user" : name;
        if (update.user_->profile_photo_) {
          std::string path = SaveAvatar(
              'u', update.user_->id_,
              update.user_->profile_photo_->minithumbnail_.get());
          if (path.empty()) path = AvatarPath('u', update.user_->id_);
          if (!path.empty()) {
            user_avatars_[update.user_->id_] = path;
            QueueAvatar(update.user_->profile_photo_->big_.get(), path);
          }
        }
      },
      [this](td_api::updateNewChat &update) {
        if (!update.chat_) return;
        chat_titles_[update.chat_->id_] = update.chat_->title_;
        chat_unread_[update.chat_->id_] =
            static_cast<uint32_t>(std::max(0, update.chat_->unread_count_));
        last_read_outbox_[update.chat_->id_] =
            update.chat_->last_read_outbox_message_id_;
        if (update.chat_->photo_) {
          std::string path = SaveAvatar(
              'c', update.chat_->id_,
              update.chat_->photo_->minithumbnail_.get());
          if (path.empty()) path = AvatarPath('c', update.chat_->id_);
          if (!path.empty()) {
            chat_avatars_[update.chat_->id_] = path;
            QueueAvatar(update.chat_->photo_->big_.get(), path);
          }
        }
        if (update.chat_->last_message_)
          chat_previews_[update.chat_->id_] = Text(*update.chat_->last_message_);
      },
      [this](td_api::updateChatTitle &update) { chat_titles_[update.chat_id_] = update.title_; },
      [this](td_api::updateChatPhoto &update) {
        if (!update.photo_) {
          chat_avatars_.erase(update.chat_id_);
          return;
        }
        std::string path = SaveAvatar(
            'c', update.chat_id_, update.photo_->minithumbnail_.get());
        if (path.empty()) path = AvatarPath('c', update.chat_id_);
        if (!path.empty()) {
          chat_avatars_[update.chat_id_] = path;
          QueueAvatar(update.photo_->big_.get(), path);
        }
      },
      [this](td_api::updateChatReadOutbox &update) {
        last_read_outbox_[update.chat_id_] =
            update.last_read_outbox_message_id_;
        const auto chat = outgoing_display_ids_.find(update.chat_id_);
        if (chat == outgoing_display_ids_.end()) return;
        for (const auto &[server_id, display_id] : chat->second) {
          if (server_id > 0 && server_id <= update.last_read_outbox_message_id_)
            Emit(tg::Kind::kMessageStatus, 2, update.chat_id_, display_id);
        }
      },
      [this](td_api::updateNewMessage &update) {
        if (!update.message_) return;
        chat_previews_[update.message_->chat_id_] = Text(*update.message_);
        EmitMessage(*update.message_);
      },
      [this](td_api::updateMessageSendSucceeded &update) {
        if (!update.message_) return;
        const int64_t chat_id = update.message_->chat_id_;
        const int64_t display_id = DisplayMessageId(
            chat_id, update.old_message_id_);
        auto &messages = outgoing_display_ids_[chat_id];
        messages.erase(update.old_message_id_);
        messages[update.message_->id_] = display_id;
        const auto read = last_read_outbox_.find(chat_id);
        const bool seen = read != last_read_outbox_.end() &&
                          update.message_->id_ <= read->second;
        Emit(tg::Kind::kMessageStatus, seen ? 2U : 1U,
             chat_id, display_id);
      },
      [this](td_api::updateMessageSendFailed &update) {
        if (!update.message_) return;
        EmitMessageStatus(update.message_->chat_id_,
                          update.old_message_id_, 3);
      },
      [this](td_api::updateDeleteMessages &update) {
        for (const int64_t message_id : update.message_ids_)
          Emit(tg::Kind::kMessageDeleted, 0, update.chat_id_,
               DisplayMessageId(update.chat_id_, message_id));
      },
      [this](td_api::updateFile &update) {
        if (!update.file_) return;
        const auto avatar = avatar_downloads_.find(update.file_->id_);
        if (avatar != avatar_downloads_.end() && update.file_->local_ &&
            update.file_->local_->is_downloading_completed_) {
          PublishAvatar(*update.file_, avatar->second);
          avatar_downloads_.erase(avatar);
        }
        const auto preview = photo_previews_.find(update.file_->id_);
        if (preview != photo_previews_.end() && update.file_->local_ &&
            update.file_->local_->is_downloading_completed_ &&
            !update.file_->local_->path_.empty()) {
          if (CopyTrustedMedia(update.file_->local_->path_,
                               preview->second.target,
                               64 * 1024 * 1024))
            Emit(tg::Kind::kPhotoReady, 0,
                 preview->second.attachment_file_id, 0,
                 preview->second.target);
          photo_previews_.erase(preview);
        }
        const auto pending = downloads_.find(update.file_->id_);
        if (pending == downloads_.end() || !update.file_->local_) return;
        const int64_t total = std::max(update.file_->size_,
                                       update.file_->expected_size_);
        const int64_t complete = update.file_->local_->downloaded_size_;
        const uint32_t percent = total > 0
            ? static_cast<uint32_t>(std::clamp<int64_t>(
                  complete * 100 / total, 0, 99))
            : 0;
        Emit(tg::Kind::kFileProgress, percent, update.file_->id_, 0,
             pending->second);
      },
      [](auto &) {}
    ));
  }
};

int main(int argc, char **argv) {
  if (argc != 2 || strcmp(argv[1], "--isolated-ipc-v1") ||
      fcntl(4, F_GETFD) < 0) return 78;
  return Worker().Run();
}
