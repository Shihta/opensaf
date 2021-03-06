/*      -*- OpenSAF  -*-
 *
 * (C) Copyright 2016 The OpenSAF Foundation
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE. This file and program are licensed
 * under the GNU Lesser General Public License Version 2.1, February 1999.
 * The complete license can be accessed from the following location:
 * http://opensource.org/licenses/lgpl-license.php
 * See the Copying file included with the OpenSAF distribution for full
 * licensing terms.
 *
 * Author(s): Ericsson AB
 *
 */

#ifndef BASE_LOG_MESSAGE_H_
#define BASE_LOG_MESSAGE_H_

#include <time.h>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <list>
#include <set>
#include <string>
#include "base/buffer.h"
#include "base/osaf_time.h"
#include "base/time.h"

namespace base {

template <size_t Capacity>
class Buffer;

// The LogMessage class implements support for formatting log records according
// to RFC 5424
class LogMessage {
 public:
  class String {
   public:
    String(const char* str, const std::string::size_type size)
        : str_{size != 0 ? str : "-", size != 0 ? size : 1} {}
    String(const std::string& str, const std::string::size_type max_size)
        : str_{str.empty() ? std::string{"-"} : str.substr(0, max_size)} {}
    bool empty() const { return str_.empty(); }
    const char* c_str() const { return str_.c_str(); }
    const char* data() const { return str_.data(); }
    const std::string::size_type size() const { return str_.size(); }
    bool operator<(const String& str) const { return str_ < str.str_; }
    bool operator==(const String& str) const { return str_ == str.str_; }

   protected:
    constexpr static const bool IsPrintableAscii(char c) {
      return c >= 33 && c <= 126;
    }
    std::string str_;
  };
  // std::string extended with a maximum string length and a limitation that all
  // characters must be printable ASCII. Empty strings are not allowed. When
  // creating an object of this class, the string is truncated if it exceeds the
  // maximum string length. Characters outside the range of printable ASCII will
  // be replaced with underscore characters (_). If the string is empty, it will
  // be replaced with a single dash (-).
  template <std::string::size_type MaxSize>
  class PrintableAscii : public String {
   public:
    explicit PrintableAscii(const std::string& str) : String{str, MaxSize} {
      for (char& c : str_)
        if (!IsPrintableAscii(c)) c = '_';
    }
  };
  // std::string extended with a maximum string length of 32 characters and a
  // limitation that all characters must be printable ASCII. Empty strings are
  // not allowed. The characters =, ] and " are not allowed. When creating an
  // object of this class, the string is truncated if it exceeds the maximum
  // string length. Illegal characters will be replaced with underscore
  // characters (_). If the string is empty, it will be replaced with a single
  // dash (-).
  class SdName : public String {
   public:
    constexpr static const std::string::size_type kMaxSize = 32;
    explicit SdName(const std::string& sd_name) : String{sd_name, kMaxSize} {
      for (char& c : str_) {
        if (!IsPrintableAscii(c) || c == '=' || c == ']' || c == '"') c = '_';
      }
    }
  };
  // Host name where the log record was created. Maximum length is 255 printable
  // ASCII characters.
  using HostName = PrintableAscii<255>;
  // Name of the application that created the log record. Maximum length is 48
  // printable ASCII characters.
  using AppName = PrintableAscii<48>;
  // Process if of the application that created the log record. Maximum length
  // is 128 printable ASCII characters.
  using ProcId = PrintableAscii<128>;
  // Message id of this log record. Maximum length is 32 printable ASCII
  // characters.
  using MsgId = PrintableAscii<32>;
  // The facility that produced this log record.
  enum class Facility {
    kKern = 0,
    kUser = 1,
    kMail = 2,
    kDaemon = 3,
    kAuth = 4,
    kSyslog = 5,
    kLpr = 6,
    kNews = 7,
    kUucp = 8,
    kCron = 9,
    kAuthPriv = 10,
    kFtp = 11,
    kNtp = 12,
    kAudit = 13,
    kAlert = 14,
    kClock = 15,
    kLocal0 = 16,
    kLocal1 = 17,
    kLocal2 = 18,
    kLocal3 = 19,
    kLocal4 = 20,
    kLocal5 = 21,
    kLocal6 = 22,
    kLocal7 = 23
  };
  // The severity level of this log record.
  enum class Severity {
    kEmerg = 0,
    kAlert = 1,
    kCrit = 2,
    kErr = 3,
    kWarning = 4,
    kNotice = 5,
    kInfo = 6,
    kDebug = 7
  };
  // A parameter/value pair for a structued element
  class Parameter {
   public:
    Parameter(const SdName& name, const std::string& value)
        : name_{name}, value_{value} {}
    template <size_t Capacity>
    void Write(Buffer<Capacity>* buffer) const;
    bool operator==(const Parameter& param) const {
      return name_ == param.name_ && value_ == param.value_;
    }

   private:
    SdName name_;
    std::string value_;
  };
  // A list of parameter/value pairs for a structued element
  using ParameterList = std::list<Parameter>;
  // A stuctured element consisting of an identifier and a list of
  // parameter/value pairs.
  class Element {
   public:
    Element(const SdName& id, const ParameterList& parameter_list)
        : id_{id}, parameter_list_{parameter_list} {}
    template <size_t Capacity>
    void Write(Buffer<Capacity>* buffer) const;
    bool operator<(const Element& elem) const { return id_ < elem.id_; }
    bool operator==(const Element& elem) const {
      return id_ == elem.id_ && parameter_list_ == elem.parameter_list_;
    }

   private:
    SdName id_;
    ParameterList parameter_list_;
  };
  // A set of stuctured elements. Each element must have a unique identifier.
  using StructuredElements = std::set<Element>;
  // kNullTime is intended to be used when the time the log record was generated
  // is not known.
  static const struct timespec kNullTime;
  // Format a log record according to rfc5424 and write it to the provided
  // buffer.
  template <size_t Capacity>
  static void Write(Facility facility, Severity severity,
                    const struct timespec& time_stamp,
                    const HostName& host_name, const AppName& app_name,
                    const ProcId& proc_id, const MsgId& msg_id,
                    const StructuredElements& structured_elements,
                    const std::string& message, Buffer<Capacity>* buffer);
  template <size_t Capacity>
  static void Write(Facility facility, Severity severity,
                    const struct timespec& time_stamp,
                    const HostName& host_name, const AppName& app_name,
                    const ProcId& proc_id, const MsgId& msg_id,
                    const StructuredElements& structured_elements,
                    const char* format, va_list ap, Buffer<Capacity>* buffer);
  template <size_t Capacity>
  static void WriteTime(const struct timespec& ts, Buffer<Capacity>* buffer);
};

template <size_t Capacity>
void LogMessage::Write(Facility facility, Severity severity,
                       const struct timespec& time_stamp,
                       const HostName& host_name, const AppName& app_name,
                       const ProcId& proc_id, const MsgId& msg_id,
                       const StructuredElements& structured_elements,
                       const std::string& message, Buffer<Capacity>* buffer) {
  uint32_t priority = static_cast<uint32_t>(facility) * uint32_t{8} +
                      static_cast<uint32_t>(severity);
  buffer->AppendChar('<');
  buffer->AppendNumber(priority, 100);
  buffer->AppendString(">1 ", 3);
  WriteTime(time_stamp, buffer);
  buffer->AppendChar(' ');
  buffer->AppendString(host_name.data(), host_name.size());
  buffer->AppendChar(' ');
  buffer->AppendString(app_name.data(), app_name.size());
  buffer->AppendChar(' ');
  buffer->AppendString(proc_id.data(), proc_id.size());
  buffer->AppendChar(' ');
  buffer->AppendString(msg_id.data(), msg_id.size());
  buffer->AppendChar(' ');
  if (structured_elements.empty()) {
    buffer->AppendChar('-');
  } else {
    for (const auto& elem : structured_elements) elem.Write(buffer);
  }
  if (!message.empty()) {
    buffer->AppendChar(' ');
    buffer->AppendString(message.data(), message.size());
  }
}

template <size_t Capacity>
void LogMessage::Write(Facility facility, Severity severity,
                       const struct timespec& time_stamp,
                       const HostName& host_name, const AppName& app_name,
                       const ProcId& proc_id, const MsgId& msg_id,
                       const StructuredElements& structured_elements,
                       const char* format, va_list ap,
                       Buffer<Capacity>* buffer) {
  uint32_t priority = static_cast<uint32_t>(facility) * uint32_t{8} +
                      static_cast<uint32_t>(severity);
  buffer->AppendChar('<');
  buffer->AppendNumber(priority, 100);
  buffer->AppendString(">1 ", 3);
  WriteTime(time_stamp, buffer);
  buffer->AppendChar(' ');
  buffer->AppendString(host_name.data(), host_name.size());
  buffer->AppendChar(' ');
  buffer->AppendString(app_name.data(), app_name.size());
  buffer->AppendChar(' ');
  buffer->AppendString(proc_id.data(), proc_id.size());
  buffer->AppendChar(' ');
  buffer->AppendString(msg_id.data(), msg_id.size());
  buffer->AppendChar(' ');
  if (structured_elements.empty()) {
    buffer->AppendChar('-');
  } else {
    for (const auto& elem : structured_elements) elem.Write(buffer);
  }
  if (format[0] != '\0') {
    buffer->AppendChar(' ');
    char* buf = buffer->end();
    size_t size = buffer->capacity() - buffer->size();
    int n = vsnprintf(buf, size, format, ap);
    if (n >= 0) {
      if (static_cast<size_t>(n) < size) size = n;
      buffer->set_size(buffer->size() + size);
    }
  }
}

template <size_t Capacity>
void LogMessage::WriteTime(const struct timespec& ts,
                           Buffer<Capacity>* buffer) {
  struct tm local_time;
  struct tm* local_ptr = localtime_r(&ts.tv_sec, &local_time);
  if (local_ptr != nullptr && local_ptr->tm_year >= -1900 &&
      local_ptr->tm_year <= (9999 - 1900) && ts.tv_nsec >= 0 &&
      ts.tv_nsec < kNanosPerSec) {
    buffer->AppendFixedWidthNumber(local_ptr->tm_year + 1900, 1000);
    buffer->AppendChar('-');
    buffer->AppendFixedWidthNumber(local_ptr->tm_mon + 1, 10);
    buffer->AppendChar('-');
    buffer->AppendFixedWidthNumber(local_ptr->tm_mday, 10);
    buffer->AppendChar('T');
    buffer->AppendFixedWidthNumber(local_ptr->tm_hour, 10);
    buffer->AppendChar(':');
    buffer->AppendFixedWidthNumber(local_ptr->tm_min, 10);
    buffer->AppendChar(':');
    buffer->AppendFixedWidthNumber(local_ptr->tm_sec, 10);
    uint32_t decimals = ts.tv_nsec / 1000;
    if (decimals != 0) {
      uint32_t power = 100000;
      while (decimals % 10 == 0) {
        decimals /= 10;
        power /= 10;
      }
      buffer->AppendChar('.');
      buffer->AppendFixedWidthNumber(decimals, power);
    }
    char* buf = buffer->end();
    if ((buffer->capacity() - buffer->size()) >= 6 &&
        strftime(buf, 6, "%z", local_ptr) == 5) {
      if (buf[1] != '0' || buf[2] != '0' || buf[3] != '0' || buf[4] != '0') {
        buf[5] = buf[4];
        buf[4] = buf[3];
        buf[3] = ':';
        buffer->set_size(buffer->size() + 6);
      } else {
        buffer->AppendChar('Z');
      }
    }
  } else {
    buffer->AppendChar('-');
  }
}

template <size_t Capacity>
void LogMessage::Element::Write(Buffer<Capacity>* buffer) const {
  buffer->AppendChar('[');
  buffer->AppendString(id_.data(), id_.size());
  for (const Parameter& param : parameter_list_) {
    buffer->AppendChar(' ');
    param.Write(buffer);
  }
  buffer->AppendChar(']');
}

template <size_t Capacity>
void LogMessage::Parameter::Write(Buffer<Capacity>* buffer) const {
  buffer->AppendString(name_.data(), name_.size());
  buffer->AppendChar('=');
  buffer->AppendChar('"');
  for (const char& c : value_) {
    if (c == '"' || c == '\\' || c == ']') buffer->AppendChar('\\');
    buffer->AppendChar(c);
  }
  buffer->AppendChar('"');
}

}  // namespace base

#endif  // BASE_LOG_MESSAGE_H_
