// SPDX-License-Identifier: Boost-1.0
#include "protocol.hpp"

#include <td/telegram/Client.h>
#include <td/telegram/td_api.h>
#include <td/telegram/td_api.hpp>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <functional>
#include <fcntl.h>
#include <map>
#include <memory>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <unistd.h>

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
      case td_api::messagePhoto::ID: return "Photo";
      case td_api::messageVideo::ID: return "Video";
      case td_api::messageAudio::ID: return "Audio";
      case td_api::messageVoiceNote::ID: return "Voice message";
      case td_api::messageDocument::ID: return "Document";
      case td_api::messageSticker::ID: return "Sticker";
      default: return "Message";
    }
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

  void EmitMessage(const td_api::message &message) {
    std::string payload = Sender(message);
    payload.push_back('\n');
    payload += Text(message);
    Emit(tg::Kind::kMessage, message.is_outgoing_ ? 1U : 0U,
         message.chat_id_, message.id_, payload);
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
        api_id_ = static_cast<int32_t>(message.primary);
        api_hash_ = message.text;
        if (const auto split = api_hash_.find('\n'); split != std::string::npos) {
          database_key_ = api_hash_.substr(split + 1);
          api_hash_.resize(split);
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
            Emit(tg::Kind::kChat, 0, id, 0,
                 it == chat_titles_.end() ? "Telegram chat" : it->second);
          }
          Emit(tg::Kind::kChatsDone);
        });
        break;
      case tg::Kind::kOpenChat: {
        const int64_t chat_id = message.primary;
        Send(td_api::make_object<td_api::getChatHistory>(chat_id, 0, 0, 50, false),
             [this](Object object) {
          if (!object || object->get_id() == td_api::error::ID) {
            Error("Could not load this conversation"); return;
          }
          auto history = td::move_tl_object_as<td_api::messages>(object);
          for (auto it = history->messages_.rbegin(); it != history->messages_.rend(); ++it)
            if (*it) EmitMessage(**it);
          Emit(tg::Kind::kMessagesDone);
        });
        break;
      }
      case tg::Kind::kSendText: {
        auto request = td_api::make_object<td_api::sendMessage>();
        request->chat_id_ = message.primary;
        auto content = td_api::make_object<td_api::inputMessageText>();
        content->text_ = td_api::make_object<td_api::formattedText>(message.text, td_api::array<td_api::object_ptr<td_api::textEntity>>{});
        request->input_message_content_ = std::move(content);
        Send(std::move(request), [this](Object object) {
          if (!object || object->get_id() == td_api::error::ID) Error("Message could not be sent");
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
            SendState(tg::AuthState::kNeedConfiguration,
                      "Enter your Telegram API credentials and an AERA vault password");
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
      },
      [this](td_api::updateNewChat &update) {
        if (update.chat_) chat_titles_[update.chat_->id_] = update.chat_->title_;
      },
      [this](td_api::updateChatTitle &update) { chat_titles_[update.chat_id_] = update.title_; },
      [this](td_api::updateNewMessage &update) { if (update.message_) EmitMessage(*update.message_); },
      [](auto &) {}
    ));
  }
};

int main(int argc, char **argv) {
  if (argc != 2 || strcmp(argv[1], "--isolated-ipc-v1") ||
      fcntl(4, F_GETFD) < 0) return 78;
  return Worker().Run();
}
