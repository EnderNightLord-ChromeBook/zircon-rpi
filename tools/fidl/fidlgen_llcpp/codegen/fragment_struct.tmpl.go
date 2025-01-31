// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package codegen

const fragmentStructTmpl = `
{{- define "StructForwardDeclaration" }}
{{ EnsureNamespace . }}
struct {{ .Name }};
{{- EnsureNamespace .WireAlias }}
using {{ .WireAlias.Name }} = {{ . }};
{{- end }}

{{- define "SentSize" }}
  {{- if gt .MaxSentSize 65536 -}}
  ZX_CHANNEL_MAX_MSG_BYTES
  {{- else -}}
  FIDL_ALIGN(PrimarySize + MaxOutOfLine)
  {{- end -}}
{{- end }}

{{/* TODO(fxbug.dev/36441): Remove __Fuchsia__ ifdefs once we have non-Fuchsia
     emulated handles for C++. */}}
{{- define "StructDeclaration" }}
{{ EnsureNamespace . }}
{{ if .IsResourceType }}
#ifdef __Fuchsia__
{{- PushNamespace }}
{{- end }}
extern "C" const fidl_type_t {{ .TableType }};
{{ range .DocComments }}
//{{ . }}
{{- end }}
struct {{ .Name }} {
  static constexpr const fidl_type_t* Type = &{{ .TableType }};
  static constexpr uint32_t MaxNumHandles = {{ .MaxHandles }};
  static constexpr uint32_t PrimarySize = {{ .InlineSize }};
  [[maybe_unused]]
  static constexpr uint32_t MaxOutOfLine = {{ .MaxOutOfLine }};
  static constexpr bool HasPointer = {{ .HasPointer }};

  {{- range .Members }}
{{ "" }}
  {{- range .DocComments }}
  //{{ . }}
  {{- end }}
  {{ .Type }} {{ .Name }} = {};
  {{- end }}

  {{- if .IsResourceType }}

  void _CloseHandles();
  {{- end }}

  private:
  class UnownedEncodedByteMessage final {
   public:
    UnownedEncodedByteMessage(uint8_t* bytes, uint32_t byte_size, {{ .Name }}* value)
        : message_(bytes, byte_size, sizeof({{ .Name }}),
    {{- if gt .MaxHandles 0 }}
      handles_, std::min(ZX_CHANNEL_MAX_MSG_HANDLES, MaxNumHandles), 0
    {{- else }}
      nullptr, 0, 0
    {{- end }}
      ) {
      message_.Encode<{{ .Name }}>(value);
    }
    UnownedEncodedByteMessage(const UnownedEncodedByteMessage&) = delete;
    UnownedEncodedByteMessage(UnownedEncodedByteMessage&&) = delete;
    UnownedEncodedByteMessage* operator=(const UnownedEncodedByteMessage&) = delete;
    UnownedEncodedByteMessage* operator=(UnownedEncodedByteMessage&&) = delete;

    zx_status_t status() const { return message_.status(); }
#ifdef __Fuchsia__
    const char* status_string() const { return message_.status_string(); }
#endif
    bool ok() const { return message_.status() == ZX_OK; }
    const char* error() const { return message_.error(); }

    ::fidl::OutgoingByteMessage& GetOutgoingMessage() { return message_; }

   private:
    {{- if gt .MaxHandles 0 }}
      zx_handle_disposition_t handles_[std::min(ZX_CHANNEL_MAX_MSG_HANDLES, MaxNumHandles)];
    {{- end }}
    ::fidl::OutgoingByteMessage message_;
  };

  class UnownedEncodedIovecMessage final {
   public:
    UnownedEncodedIovecMessage(
      zx_channel_iovec_t* iovecs, uint32_t iovec_size,
      fidl_iovec_substitution_t* substitutions, uint32_t substitutions_size,
      {{ .Name }}* value)
        : message_(::fidl::OutgoingIovecMessage::constructor_args{
          .iovecs = iovecs,
          .iovecs_actual = 0,
          .iovecs_capacity = iovec_size,
          .substitutions = substitutions,
          .substitutions_actual = 0,
          .substitutions_capacity = substitutions_size,
          {{- if gt .MaxHandles 0 }}
          .handles = handles_,
          .handle_actual = 0,
          .handle_capacity = std::min(ZX_CHANNEL_MAX_MSG_HANDLES, MaxNumHandles),
          {{- else }}
          .handles = nullptr,
          .handle_actual = 0,
          .handle_capacity = 0,
          {{- end }}
        }) {
      message_.Encode<{{ .Name }}>(value);
    }
    UnownedEncodedIovecMessage(const UnownedEncodedIovecMessage&) = delete;
    UnownedEncodedIovecMessage(UnownedEncodedIovecMessage&&) = delete;
    UnownedEncodedIovecMessage* operator=(const UnownedEncodedIovecMessage&) = delete;
    UnownedEncodedIovecMessage* operator=(UnownedEncodedIovecMessage&&) = delete;

    zx_status_t status() const { return message_.status(); }
#ifdef __Fuchsia__
    const char* status_string() const { return message_.status_string(); }
#endif
    bool ok() const { return message_.status() == ZX_OK; }
    const char* error() const { return message_.error(); }

    ::fidl::OutgoingIovecMessage& GetOutgoingMessage() { return message_; }

   private:
    {{- if gt .MaxHandles 0 }}
      zx_handle_disposition_t handles_[std::min(ZX_CHANNEL_MAX_MSG_HANDLES, MaxNumHandles)];
    {{- end }}
    ::fidl::OutgoingIovecMessage message_;
  };

  class OwnedEncodedByteMessage final {
   public:
    explicit OwnedEncodedByteMessage({{ .Name }}* value)
        {{- if gt .MaxSentSize 512 -}}
      : bytes_(std::make_unique<::fidl::internal::AlignedBuffer<{{- template "SentSize" . }}>>()),
        message_(bytes_->data(), {{- template "SentSize" . }}
        {{- else }}
        : message_(bytes_, sizeof(bytes_)
        {{- end }}
        , value) {}
    OwnedEncodedByteMessage(const OwnedEncodedByteMessage&) = delete;
    OwnedEncodedByteMessage(OwnedEncodedByteMessage&&) = delete;
    OwnedEncodedByteMessage* operator=(const OwnedEncodedByteMessage&) = delete;
    OwnedEncodedByteMessage* operator=(OwnedEncodedByteMessage&&) = delete;

    zx_status_t status() const { return message_.status(); }
#ifdef __Fuchsia__
    const char* status_string() const { return message_.status_string(); }
#endif
    bool ok() const { return message_.ok(); }
    const char* error() const { return message_.error(); }

    ::fidl::OutgoingByteMessage& GetOutgoingMessage() { return message_.GetOutgoingMessage(); }

   private:
    {{- if gt .MaxSentSize 512 }}
    std::unique_ptr<::fidl::internal::AlignedBuffer<{{- template "SentSize" . }}>> bytes_;
    {{- else }}
    FIDL_ALIGNDECL
    uint8_t bytes_[FIDL_ALIGN(PrimarySize + MaxOutOfLine)];
    {{- end }}
    UnownedEncodedByteMessage message_;
  };

  class OwnedEncodedIovecMessage final {
   public:
    explicit OwnedEncodedIovecMessage({{ .Name }}* value)
        : message_(iovecs_, ::fidl::internal::kIovecBufferSize,
        substitutions_, ::fidl::internal::kIovecBufferSize,
        value) {}
    OwnedEncodedIovecMessage(const OwnedEncodedIovecMessage&) = delete;
    OwnedEncodedIovecMessage(OwnedEncodedIovecMessage&&) = delete;
    OwnedEncodedIovecMessage* operator=(const OwnedEncodedIovecMessage&) = delete;
    OwnedEncodedIovecMessage* operator=(OwnedEncodedIovecMessage&&) = delete;

    zx_status_t status() const { return message_.status(); }
#ifdef __Fuchsia__
    const char* status_string() const { return message_.status_string(); }
#endif
    bool ok() const { return message_.ok(); }
    const char* error() const { return message_.error(); }

    ::fidl::OutgoingIovecMessage& GetOutgoingMessage() { return message_.GetOutgoingMessage(); }

   private:
    zx_channel_iovec_t iovecs_[::fidl::internal::kIovecBufferSize];
    fidl_iovec_substitution_t substitutions_[::fidl::internal::kIovecBufferSize];
    UnownedEncodedIovecMessage message_;
  };

  public:
    friend ::fidl::internal::EncodedMessageTypes<{{ .Name }}>;
    using OwnedEncodedMessage = OwnedEncodedByteMessage;
    using UnownedEncodedMessage = UnownedEncodedByteMessage;

  class DecodedMessage final : public ::fidl::internal::IncomingMessage {
   public:
    DecodedMessage(uint8_t* bytes, uint32_t byte_actual, zx_handle_info_t* handles = nullptr,
                    uint32_t handle_actual = 0)
        : ::fidl::internal::IncomingMessage(bytes, byte_actual, handles, handle_actual) {
      Decode<struct {{ .Name }}>();
    }
    DecodedMessage(fidl_incoming_msg_t* msg) : ::fidl::internal::IncomingMessage(msg) {
      Decode<struct {{ .Name }}>();
    }
    DecodedMessage(const DecodedMessage&) = delete;
    DecodedMessage(DecodedMessage&&) = delete;
    DecodedMessage* operator=(const DecodedMessage&) = delete;
    DecodedMessage* operator=(DecodedMessage&&) = delete;
    {{- if .IsResourceType }}
    ~DecodedMessage() {
      if (ok() && (PrimaryObject() != nullptr)) {
        PrimaryObject()->_CloseHandles();
      }
    }
    {{- end }}

    struct {{ .Name }}* PrimaryObject() {
      ZX_DEBUG_ASSERT(ok());
      return reinterpret_cast<struct {{ .Name }}*>(bytes());
    }

    // Release the ownership of the decoded message. That means that the handles won't be closed
    // When the object is destroyed.
    // After calling this method, the DecodedMessage object should not be used anymore.
    void ReleasePrimaryObject() { ResetBytes(); }
  };
};
{{- if .IsResourceType }}
{{- PopNamespace }}
#endif  // __Fuchsia__
{{- end }}
{{- end }}

{{/* TODO(fxbug.dev/36441): Remove __Fuchsia__ ifdefs once we have non-Fuchsia
     emulated handles for C++. */}}
{{- define "StructDefinition" }}
{{ EnsureNamespace "::" }}
{{ if .IsResourceType }}
#ifdef __Fuchsia__
{{- PushNamespace }}
void {{ . }}::_CloseHandles() {
  {{- range .Members }}
    {{- CloseHandles . false false }}
  {{- end }}
}
{{- PopNamespace }}
#endif  // __Fuchsia__
{{- end }}
{{- end }}

{{/* TODO(fxbug.dev/36441): Remove __Fuchsia__ ifdefs once we have non-Fuchsia
     emulated handles for C++. */}}
{{- define "StructTraits" }}
{{ if .IsResourceType }}
#ifdef __Fuchsia__
{{- PushNamespace }}
{{- end }}
template <>
struct IsFidlType<{{ . }}> : public std::true_type {};
template <>
struct IsStruct<{{ . }}> : public std::true_type {};
static_assert(std::is_standard_layout_v<{{ . }}>);
{{- $struct := . }}
{{- range .Members }}
static_assert(offsetof({{ $struct }}, {{ .Name }}) == {{ .Offset }});
{{- end }}
static_assert(sizeof({{ . }}) == {{ . }}::PrimarySize);
{{- if .IsResourceType }}
{{- PopNamespace }}
#endif  // __Fuchsia__
{{- end }}
{{- end }}
`
