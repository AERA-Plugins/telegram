// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <cstring>

namespace aera::telegram {

constexpr uint32_t kMagic = 0x4154474dU;  // ATGM
constexpr uint32_t kProtocolVersion = 1;
constexpr size_t kTextBytes = 2048;

enum class Kind : uint32_t {
  // Trusted UI -> isolated TDLib worker.
  kConfigure = 1,
  kPhone,
  kCode,
  kPassword,
  kEmail,
  kEmailCode,
  kRegister,
  kLoadChats,
  kOpenChat,
  kSendText,
  kClose,

  // Isolated worker -> trusted UI.
  kState = 64,
  kStatus,
  kError,
  kChat,
  kChatsDone,
  kMessage,
  kMessagesDone,
};

enum class AuthState : uint32_t {
  kStarting = 0,
  kNeedConfiguration,
  kNeedPhone,
  kNeedCode,
  kNeedPassword,
  kNeedEmail,
  kNeedEmailCode,
  kNeedRegistration,
  kConfirmElsewhere,
  kReady,
  kClosing,
};

struct Message {
  uint32_t magic = kMagic;
  uint32_t version = kProtocolVersion;
  Kind kind = Kind::kStatus;
  uint32_t value = 0;
  int64_t primary = 0;
  int64_t secondary = 0;
  char text[kTextBytes]{};
};

static_assert(sizeof(Message) == 2080);

inline bool HasTerminator(const Message &message) {
  return memchr(message.text, '\0', sizeof(message.text)) != nullptr;
}

inline bool Valid(const Message &message, bool from_worker) {
  if (message.magic != kMagic || message.version != kProtocolVersion ||
      !HasTerminator(message)) {
    return false;
  }
  if (from_worker) {
    return message.kind == Kind::kState || message.kind == Kind::kStatus ||
           message.kind == Kind::kError || message.kind == Kind::kChat ||
           message.kind == Kind::kChatsDone ||
           message.kind == Kind::kMessage ||
           message.kind == Kind::kMessagesDone;
  }
  return message.kind >= Kind::kConfigure && message.kind <= Kind::kClose;
}

}  // namespace aera::telegram
