// Copyright 2017 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/asmjs/asm-scanner.h"

#include "src/conversions.h"
#include "src/flags.h"
#include "src/parsing/scanner.h"
#include "src/unicode-cache.h"

namespace v8 {
namespace internal {

namespace {
// Cap number of identifiers to ensure we can assign both global and
// local ones a token id in the range of an int32_t.
static const int kMaxIdentifierCount = 0xf000000;
};

AsmJsScanner::AsmJsScanner()
    : token_(kUninitialized),
      preceding_token_(kUninitialized),
      next_token_(kUninitialized),
      position_(0),
      preceding_position_(0),
      next_position_(0),
      rewind_(false),
      in_local_scope_(false),
      global_count_(0),
      double_value_(0.0),
      unsigned_value_(0),
      preceded_by_newline_(false) {
#define V(name, _junk1, _junk2, _junk3) property_names_[#name] = kToken_##name;
  STDLIB_MATH_FUNCTION_LIST(V)
  STDLIB_ARRAY_TYPE_LIST(V)
#undef V
#define V(name, _junk1) property_names_[#name] = kToken_##name;
  STDLIB_MATH_VALUE_LIST(V)
#undef V
#define V(name) property_names_[#name] = kToken_##name;
  STDLIB_OTHER_LIST(V)
#undef V
#define V(name) global_names_[#name] = kToken_##name;
  KEYWORD_NAME_LIST(V)
#undef V
}

void AsmJsScanner::SetStream(std::unique_ptr<Utf16CharacterStream> stream) {
  stream_ = std::move(stream);
  Next();
}

void AsmJsScanner::Next() {
  if (rewind_) {
    preceding_token_ = token_;
    preceding_position_ = position_;
    token_ = next_token_;
    position_ = next_position_;
    next_token_ = kUninitialized;
    next_position_ = 0;
    rewind_ = false;
    return;
  }

  if (token_ == kEndOfInput || token_ == kParseError) {
    return;
  }

#if DEBUG
  if (FLAG_trace_asm_scanner) {
    if (Token() == kDouble) {
      PrintF("%lf ", AsDouble());
    } else if (Token() == kUnsigned) {
      PrintF("%" PRIu32 " ", AsUnsigned());
    } else {
      std::string name = Name(Token());
      PrintF("%s ", name.c_str());
    }
  }
#endif

  preceded_by_newline_ = false;
  preceding_token_ = token_;
  preceding_position_ = position_;

  for (;;) {
    position_ = stream_->pos();
    uc32 ch = stream_->Advance();
    switch (ch) {
      case ' ':
      case '\t':
      case '\r':
        // Ignore whitespace.
        break;

      case '\n':
        // Track when we've passed a newline for optional semicolon support,
        // but keep scanning.
        preceded_by_newline_ = true;
        break;

      case kEndOfInput:
        token_ = kEndOfInput;
        return;

      case '\'':
      case '"':
        ConsumeString(ch);
        return;

      case '/':
        ch = stream_->Advance();
        if (ch == '/') {
          ConsumeCPPComment();
        } else if (ch == '*') {
          if (!ConsumeCComment()) {
            token_ = kParseError;
            return;
          }
        } else {
          stream_->Back();
          token_ = '/';
          return;
        }
        // Breaks out of switch, but loops again (i.e. the case when we parsed
        // a comment, but need to continue to look for the next token).
        break;

      case '<':
      case '>':
      case '=':
      case '!':
        ConsumeCompareOrShift(ch);
        return;

#define V(single_char_token) case single_char_token:
        SIMPLE_SINGLE_TOKEN_LIST(V)
#undef V
        // Use fixed token IDs for ASCII.
        token_ = ch;
        return;

      default:
        if (IsIdentifierStart(ch)) {
          ConsumeIdentifier(ch);
        } else if (IsNumberStart(ch)) {
          ConsumeNumber(ch);
        } else {
          // TODO(bradnelson): Support unicode (probably via UnicodeCache).
          token_ = kParseError;
        }
        return;
    }
  }
}

void AsmJsScanner::Rewind() {
  DCHECK_NE(kUninitialized, preceding_token_);
  // TODO(bradnelson): Currently rewinding needs to leave in place the
  // preceding newline state (in case a |0 ends a line).
  // This is weird and stateful, fix me.
  DCHECK(!rewind_);
  next_token_ = token_;
  next_position_ = position_;
  token_ = preceding_token_;
  position_ = preceding_position_;
  preceding_token_ = kUninitialized;
  preceding_position_ = 0;
  rewind_ = true;
  identifier_string_.clear();
}

void AsmJsScanner::ResetLocals() { local_names_.clear(); }

#if DEBUG
// Only used for debugging.
std::string AsmJsScanner::Name(token_t token) const {
  if (token >= 32 && token < 127) {
    return std::string(1, static_cast<char>(token));
  }
  for (auto& i : local_names_) {
    if (i.second == token) {
      return i.first;
    }
  }
  for (auto& i : global_names_) {
    if (i.second == token) {
      return i.first;
    }
  }
  for (auto& i : property_names_) {
    if (i.second == token) {
      return i.first;
    }
  }
  switch (token) {
#define V(rawname, name) \
  case kToken_##name:    \
    return rawname;
    LONG_SYMBOL_NAME_LIST(V)
#undef V
#define V(name, value, string_name) \
  case name:                        \
    return string_name;
    SPECIAL_TOKEN_LIST(V)
    default:
      break;
  }
  UNREACHABLE();
  return "{unreachable}";
}
#endif

void AsmJsScanner::Seek(size_t pos) {
  stream_->Seek(pos);
  preceding_token_ = kUninitialized;
  token_ = kUninitialized;
  next_token_ = kUninitialized;
  preceding_position_ = 0;
  position_ = 0;
  next_position_ = 0;
  rewind_ = false;
  Next();
}

void AsmJsScanner::ConsumeIdentifier(uc32 ch) {
  // Consume characters while still part of the identifier.
  identifier_string_.clear();
  while (IsIdentifierPart(ch)) {
    identifier_string_ += ch;
    ch = stream_->Advance();
  }
  // Go back one for next time.
  stream_->Back();

  // Decode what the identifier means.
  if (preceding_token_ == '.') {
    auto i = property_names_.find(identifier_string_);
    if (i != property_names_.end()) {
      token_ = i->second;
      return;
    }
  } else {
    {
      auto i = local_names_.find(identifier_string_);
      if (i != local_names_.end()) {
        token_ = i->second;
        return;
      }
    }
    if (!in_local_scope_) {
      auto i = global_names_.find(identifier_string_);
      if (i != global_names_.end()) {
        token_ = i->second;
        return;
      }
    }
  }
  if (preceding_token_ == '.') {
    CHECK(global_count_ < kMaxIdentifierCount);
    token_ = kGlobalsStart + global_count_++;
    property_names_[identifier_string_] = token_;
  } else if (in_local_scope_) {
    CHECK(local_names_.size() < kMaxIdentifierCount);
    token_ = kLocalsStart - static_cast<token_t>(local_names_.size());
    local_names_[identifier_string_] = token_;
  } else {
    CHECK(global_count_ < kMaxIdentifierCount);
    token_ = kGlobalsStart + global_count_++;
    global_names_[identifier_string_] = token_;
  }
}

void AsmJsScanner::ConsumeNumber(uc32 ch) {
  std::string number;
  number = ch;
  bool has_dot = ch == '.';
  for (;;) {
    ch = stream_->Advance();
    if ((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') ||
        (ch >= 'A' && ch <= 'F') || ch == '.' || ch == 'b' || ch == 'o' ||
        ch == 'x' ||
        ((ch == '-' || ch == '+') && (number[number.size() - 1] == 'e' ||
                                      number[number.size() - 1] == 'E'))) {
      // TODO(bradnelson): Test weird cases ending in -.
      if (ch == '.') {
        has_dot = true;
      }
      number.push_back(ch);
    } else {
      break;
    }
  }
  stream_->Back();
  // Special case the most common number.
  if (number.size() == 1 && number[0] == '0') {
    unsigned_value_ = 0;
    token_ = kUnsigned;
    return;
  }
  // Pick out dot.
  if (number.size() == 1 && number[0] == '.') {
    token_ = '.';
    return;
  }
  // Decode numbers.
  UnicodeCache cache;
  double_value_ = StringToDouble(
      &cache,
      Vector<const uint8_t>(reinterpret_cast<const uint8_t*>(number.data()),
                            static_cast<int>(number.size())),
      ALLOW_HEX | ALLOW_OCTAL | ALLOW_BINARY | ALLOW_IMPLICIT_OCTAL);
  if (std::isnan(double_value_)) {
    // Check if string to number conversion didn't consume all the characters.
    // This happens if the character filter let through something invalid
    // like: 0123ef for example.
    // TODO(bradnelson): Check if this happens often enough to be a perf
    // problem.
    if (number[0] == '.') {
      for (size_t k = 1; k < number.size(); ++k) {
        stream_->Back();
      }
      token_ = '.';
      return;
    }
    // Anything else that doesn't parse is an error.
    token_ = kParseError;
    return;
  }
  if (has_dot) {
    token_ = kDouble;
  } else {
    // Exceeding safe integer range is an error.
    if (double_value_ > static_cast<double>(kMaxUInt32)) {
      token_ = kParseError;
      return;
    }
    unsigned_value_ = static_cast<uint32_t>(double_value_);
    token_ = kUnsigned;
  }
}

bool AsmJsScanner::ConsumeCComment() {
  for (;;) {
    uc32 ch = stream_->Advance();
    while (ch == '*') {
      ch = stream_->Advance();
      if (ch == '/') {
        return true;
      }
    }
    if (ch == kEndOfInput) {
      return false;
    }
  }
}

void AsmJsScanner::ConsumeCPPComment() {
  for (;;) {
    uc32 ch = stream_->Advance();
    if (ch == '\n' || ch == kEndOfInput) {
      return;
    }
  }
}

void AsmJsScanner::ConsumeString(uc32 quote) {
  // Only string allowed is 'use asm' / "use asm".
  const char* expected = "use asm";
  for (; *expected != '\0'; ++expected) {
    if (stream_->Advance() != *expected) {
      token_ = kParseError;
      return;
    }
  }
  if (stream_->Advance() != quote) {
    token_ = kParseError;
    return;
  }
  token_ = kToken_UseAsm;
}

void AsmJsScanner::ConsumeCompareOrShift(uc32 ch) {
  uc32 next_ch = stream_->Advance();
  if (next_ch == '=') {
    switch (ch) {
      case '<':
        token_ = kToken_LE;
        break;
      case '>':
        token_ = kToken_GE;
        break;
      case '=':
        token_ = kToken_EQ;
        break;
      case '!':
        token_ = kToken_NE;
        break;
      default:
        UNREACHABLE();
    }
  } else if (ch == '<' && next_ch == '<') {
    token_ = kToken_SHL;
  } else if (ch == '>' && next_ch == '>') {
    if (stream_->Advance() == '>') {
      token_ = kToken_SHR;
    } else {
      token_ = kToken_SAR;
      stream_->Back();
    }
  } else {
    stream_->Back();
    token_ = ch;
  }
}

bool AsmJsScanner::IsIdentifierStart(uc32 ch) {
  return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || ch == '_' ||
         ch == '$';
}

bool AsmJsScanner::IsIdentifierPart(uc32 ch) {
  return IsIdentifierStart(ch) || (ch >= '0' && ch <= '9');
}

bool AsmJsScanner::IsNumberStart(uc32 ch) {
  return ch == '.' || (ch >= '0' && ch <= '9');
}

}  // namespace internal
}  // namespace v8
