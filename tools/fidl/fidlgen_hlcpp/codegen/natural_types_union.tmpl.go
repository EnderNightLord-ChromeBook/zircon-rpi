// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package codegen

const unionTemplate = `
{{- define "UnionForwardDeclaration" }}
{{ EnsureNamespace . }}
class {{ .Name }};
{{- end }}

{{/* TODO(fxbug.dev/36441): Remove __Fuchsia__ ifdefs once we have non-Fuchsia
     emulated handles for C++. */}}
{{- define "UnionDeclaration" }}
{{ EnsureNamespace . }}
{{ if .IsResourceType }}
#ifdef __Fuchsia__
{{- PushNamespace }}
{{- end }}
{{- range .DocComments }}
///{{ . }}
{{- end }}
class {{ .Name }} final {
 public:
 static const fidl_type_t* FidlType;

  {{ .Name }}();
  ~{{ .Name }}();

  {{ .Name }}({{ .Name }}&&);
  {{ .Name }}& operator=({{ .Name }}&&);

  {{ range .Members }}
  static {{ $.Name }} With{{ .UpperCamelCaseName }}({{ .Type }}&&);
  {{- end }}

  {{/* There are two different tag types here:

    * fidl_xunion_tag_t: This is an "open" enum that encompasses all possible ordinal values
      (including zero). Ordinal() returns a fidl_xunion_tag_t.
    * An inner ::Tag enum: This only contains valid ordinals for this xunion. Which() returns a
      ::Tag.

    The two types generally carry the same value. However:

    * If the ordinal is zero, which is only ever the case when the xunion is first constructed and
      not yet set:
      * tag_, which is a fidl_xunion_tag_t, will be kEmpty (which is kFidlXUnionEmptyTag, or 0).
        kEmpty is intended for internal use only; clients should use ::Tag instead.
      * Ordinal() will return kEmpty.
      * Which() will return Tag::kUnknown.
    * if the xunion is non-strict (flexible) and has been de-serialized from a xunion with an
      ordinal that's unknown to the client's schema:
      * tag_ will be the raw ordinal from the serialized xunion,
      * Ordinal() will return tag_,
      * Which() will return Tag::kUnknown.
      * UnknownBytes() will return a pointer to a valid std::vector<uint8_t> with the unknown bytes.
      * if the xunion is a resource type:
        * UnknownHandles() will return a pointer to a valid std::vector<zx::handle> with the unknown handles.
    */ -}}

  enum __attribute__((enum_extensibility(closed))) Tag : fidl_xunion_tag_t {
  {{ if .IsFlexible -}}
    kUnknown = 0,
    {{- /* TODO(fxbug.dev/8050): Remove the Empty tag below. */}}
    Empty = kUnknown,  // DEPRECATED: use kUnknown instead.
  {{ end -}}
  {{- range .Members }}
    {{ .TagName }} = {{ .Ordinal }},  // {{ .Ordinal | printf "%#x" }}
  {{- end }}
    Invalid = ::std::numeric_limits<::fidl_union_tag_t>::max(),
  };

  static inline ::std::unique_ptr<{{ .Name }}> New() { return ::std::make_unique<{{ .Name }}>(); }

  void Encode(::fidl::Encoder* encoder, size_t offset,
              fit::optional<::fidl::HandleInformation> maybe_handle_info = fit::nullopt);
  static void Decode(::fidl::Decoder* decoder, {{ .Name }}* value, size_t offset);
  zx_status_t Clone({{ .Name }}* result) const;

  bool has_invalid_tag() const {
    return tag_ == Invalid;
  }

  {{- range .Members }}

  bool is_{{ .Name }}() const { return tag_ == Tag::{{ .TagName }}; }
  {{ range .DocComments }}
  ///{{ . }}
  {{- end }}
  {{ .Type }}& {{ .Name }}() {
    EnsureStorageInitialized(Tag::{{ .TagName }});
    return {{ .StorageName }};
  }
  {{ range .DocComments }}
  ///{{ . }}
  {{- end }}
  const {{ .Type }}& {{ .Name }}() const {
    ZX_ASSERT(is_{{ .Name }}());
    return {{ .StorageName }};
  }
  {{ $.Name }}& set_{{ .Name }}({{ .Type }} value);
  {{- end }}

  {{- if .IsFlexible }}
  {{ .Name }}& SetUnknownData(fidl_xunion_tag_t ordinal, std::vector<uint8_t> bytes{{ if .IsResourceType }}, std::vector<zx::handle> handles{{ end }});
  {{- end }}

  Tag Which() const {
    {{ if .IsFlexible }}
    switch (tag_) {
      case Tag::Invalid:
      {{- range .Members }}
      case Tag::{{ .TagName }}:
      {{- end }}
        return Tag(tag_);
      default:
        return Tag::kUnknown;
    }
    {{ else }}
    return Tag(tag_);
    {{ end }}
  }

  // You probably want to use Which() method instead of Ordinal(). Use Ordinal() only when you need
  // access to the raw integral ordinal value.
  fidl_xunion_tag_t Ordinal() const {
    return tag_;
  }

{{- if .IsFlexible }}
  const std::vector<uint8_t>* UnknownBytes() const {
    if (Which() != Tag::kUnknown) {
      return nullptr;
    }
  {{- if .IsResourceType }}
    return &unknown_data_.bytes;
  {{- else }}
    return &unknown_data_;
  {{- end }}
  }

  {{- if .IsResourceType }}
  const std::vector<zx::handle>* UnknownHandles() const { 
    if (Which() != Tag::kUnknown) {
      return nullptr;
    }
    return &unknown_data_.handles;
  }
  {{- end }}
{{- end }}

  friend ::fidl::Equality<{{ . }}>;

  {{- if .Result }}
  {{ .Name }}(fit::result<{{ .Result.ValueDecl }}, {{ .Result.ErrorDecl }}>&& result) {
    ZX_ASSERT(!result.is_pending());
    if (result.is_ok()) {
      {{- if eq 0 .Result.ValueArity }}
      set_response({{ .Result.ValueStructDecl }}{});
      {{- else }}
      set_response({{ .Result.ValueStructDecl }}{result.take_value()});
      {{- end }}
    } else {
      set_err(std::move(result.take_error()));
    }
  }
  {{ .Name }}(fit::ok_result<{{ .Result.ValueDecl }}>&& result)
    : {{ .Name }}(fit::result<{{ .Result.ValueDecl }}, {{ .Result.ErrorDecl }}>(std::move(result))) { }
  {{ .Name }}(fit::error_result<{{ .Result.ErrorDecl }}>&& result)
    : {{ .Name }}(fit::result<{{ .Result.ValueDecl }}, {{ .Result.ErrorDecl }}>(std::move(result))) { }
  operator fit::result<{{ .Result.ValueDecl }}, {{ .Result.ErrorDecl }}>() && {
    if (is_err()) {
      return fit::error(err());
    }
    {{- if eq 0 .Result.ValueArity }}
    return fit::ok();
    {{- else if eq 1 .Result.ValueArity }}
    {{ .Result.ValueTupleDecl }} value_tuple = std::move(response());
    return fit::ok(std::move(std::get<0>(value_tuple)));
    {{- else }}
    return fit::ok(std::move(response()));
    {{- end }}
  }
  {{- end }}

 private:
  void Destroy();
  void EnsureStorageInitialized(::fidl_xunion_tag_t tag);

  ::fidl_xunion_tag_t tag_ = static_cast<fidl_xunion_tag_t>(Tag::Invalid);
  union {
  {{- range .Members }}
    {{ .Type }} {{ .StorageName }};
  {{- end }}
  {{- if .IsFlexible }}
    {{ if .IsResourceType }}::fidl::UnknownData{{ else }}std::vector<uint8_t>{{ end }} unknown_data_;
  {{- end }}
  };
};

inline zx_status_t Clone(const {{ . }}& value,
                         {{ . }}* result) {
  return value.Clone(result);
}

using {{ .Name }}Ptr = ::std::unique_ptr<{{ .Name }}>;
{{- if .IsResourceType }}
{{- PopNamespace }}
#endif  // __Fuchsia__
{{ end }}

{{- end }}

{{- define "UnionDefinition" }}
{{ EnsureNamespace . }}
{{- if .IsResourceType }}
#ifdef __Fuchsia__
{{- PushNamespace }}
{{- end }}
extern "C" const fidl_type_t {{ .TableType }};
const fidl_type_t* {{ .Name }}::FidlType = &{{ .TableType }};

{{ .Name }}::{{ .Name }}() {}

{{ .Name }}::~{{ .Name }}() {
  Destroy();
}

{{ .Name }}::{{ .Name }}({{ .Name }}&& other) : tag_(other.tag_) {
  switch (tag_) {
  {{- range .Members }}
    case Tag::{{ .TagName }}:
    {{- if .Type.NeedsDtor }}
      new (&{{ .StorageName }}) {{ .Type }}();
    {{- end }}
      {{ .StorageName }} = std::move(other.{{ .StorageName }});
      break;
  {{- end }}
    case static_cast<fidl_xunion_tag_t>(Tag::Invalid):
      break;
  {{- if .IsFlexible }}
    default:
      new (&unknown_data_) decltype(unknown_data_);
      unknown_data_ = std::move(other.unknown_data_);
      break;
  {{- end }}
  }
}

{{ .Name }}& {{ .Name }}::operator=({{ .Name }}&& other) {
  if (this != &other) {
    Destroy();
    tag_ = other.tag_;
    switch (tag_) {
    {{- range .Members }}
      case Tag::{{ .TagName }}:
        {{- if .Type.NeedsDtor }}
        new (&{{ .StorageName }}) {{ .Type }}();
        {{- end }}
        {{ .StorageName }} = std::move(other.{{ .StorageName }});
        break;
    {{- end }}
      case static_cast<fidl_xunion_tag_t>(Tag::Invalid):
        break;
    {{- if .IsFlexible }}
      default:
        new (&unknown_data_) decltype(unknown_data_);
        unknown_data_= std::move(other.unknown_data_);
        break;
    {{- end }}
    }
  }
  return *this;
}

{{ range .Members -}}
{{ $.Name }} {{ $.Name }}::With{{ .UpperCamelCaseName }}({{ .Type }}&& val) {
  {{ $.Name }} result;
  result.set_{{ .Name }}(std::move(val));
  return result;
}
{{ end }}

void {{ .Name }}::Encode(::fidl::Encoder* encoder, size_t offset,
                         fit::optional<::fidl::HandleInformation> maybe_handle_info) {
  const size_t length_before = encoder->CurrentLength();
  const size_t handles_before = encoder->CurrentHandleCount();

  size_t envelope_offset = 0;

  switch (Which()) {
    {{- range .Members }}
    case Tag::{{ .TagName }}: {
      envelope_offset = encoder->Alloc(::fidl::EncodingInlineSize<{{ .Type }}, ::fidl::Encoder>(encoder));      
      {{- if .HandleInformation }}
      ::fidl::Encode(encoder, &{{ .StorageName }}, envelope_offset, ::fidl::HandleInformation {
        .object_type = {{ .HandleInformation.ObjectType }},
        .rights = {{ .HandleInformation.Rights }},
      });
      {{ else -}}
      ::fidl::Encode(encoder, &{{ .StorageName }}, envelope_offset);
      {{ end -}}
      break;
    }
    {{- end }}
    {{- if .IsFlexible }}
    case Tag::kUnknown:
      {{- if .IsResourceType }}
      envelope_offset = encoder->Alloc(unknown_data_.bytes.size());
      ::fidl::EncodeUnknownDataContents(encoder, &unknown_data_, envelope_offset);
      {{- else }}
      envelope_offset = encoder->Alloc(unknown_data_.size());
      ::fidl::EncodeUnknownBytesContents(encoder, &unknown_data_, envelope_offset);
      {{- end }}
      break;
    {{- end }}
    default:
       break;
  }

  {{/* Note that encoder->GetPtr() must be called after every call to
       encoder->Alloc(), since encoder.bytes_ could be re-sized and moved.
     */ -}}

  fidl_xunion_t* xunion = encoder->GetPtr<fidl_xunion_t>(offset);
  assert(xunion->envelope.presence == FIDL_ALLOC_ABSENT);

  if (envelope_offset) {
    xunion->tag = tag_;
    xunion->envelope.num_bytes = static_cast<uint32_t>(encoder->CurrentLength() - length_before);
    xunion->envelope.num_handles = static_cast<uint32_t>(encoder->CurrentHandleCount() - handles_before);
    xunion->envelope.presence = FIDL_ALLOC_PRESENT;
  }
}

void {{ .Name }}::Decode(::fidl::Decoder* decoder, {{ .Name }}* value, size_t offset) {
  fidl_xunion_t* xunion = decoder->GetPtr<fidl_xunion_t>(offset);

  if (!xunion->envelope.data) {
    value->EnsureStorageInitialized(static_cast<fidl_xunion_tag_t>(Tag::Invalid));
    return;
  }

  value->EnsureStorageInitialized(xunion->tag);

{{ if len .Members }}
  const size_t envelope_offset = decoder->GetOffset(xunion->envelope.data);

  switch (value->tag_) {
  {{- range .Members }}
    case Tag::{{ .TagName }}:
      {{- if .Type.NeedsDtor }}
      new (&value->{{ .StorageName }}) {{ .Type }}();
      {{- end }}
      ::fidl::Decode(decoder, &value->{{ .StorageName }}, envelope_offset);
      break;
  {{- end }}
    default:
  {{ if .IsFlexible -}}
    {{- if .IsResourceType }}
      value->unknown_data_.bytes.resize(xunion->envelope.num_bytes);
      value->unknown_data_.handles.resize(xunion->envelope.num_handles);
      ::fidl::DecodeUnknownDataContents(decoder, &value->unknown_data_, envelope_offset);
    {{- else }}
      value->unknown_data_.resize(xunion->envelope.num_bytes);
      ::fidl::DecodeUnknownBytesContents(decoder, &value->unknown_data_, envelope_offset);
    {{- end }}
  {{ end -}}
      break;
  }
{{ end }}
}

zx_status_t {{ .Name }}::Clone({{ .Name }}* result) const {
  result->Destroy();
  result->tag_ = tag_;
  switch (tag_) {
    case Tag::Invalid:
      return ZX_OK;
    {{- range .Members }}
    case Tag::{{ .TagName }}:
      {{- if .Type.NeedsDtor }}
      new (&result->{{ .StorageName }}) {{ .Type }}();
      {{- end }}
      return ::fidl::Clone({{ .StorageName }}, &result->{{ .StorageName }});
    {{- end }}
    default:
    {{- if .IsFlexible }}
      new (&result->unknown_data_) decltype(unknown_data_);
      return ::fidl::Clone(unknown_data_, &result->unknown_data_);
    {{ end -}}
      return ZX_OK;
  }
}

{{- range $member := .Members }}

{{ $.Name }}& {{ $.Name }}::set_{{ .Name }}({{ .Type }} value) {
  EnsureStorageInitialized(Tag::{{ .TagName }});
  {{ .StorageName }} = std::move(value);
  return *this;
}

{{- end }}

{{- if .IsFlexible }}
{{ .Name }}& {{ .Name }}::SetUnknownData(fidl_xunion_tag_t ordinal, std::vector<uint8_t> bytes{{ if .IsResourceType }}, std::vector<zx::handle> handles{{ end }}) {
  EnsureStorageInitialized(ordinal);
  {{- if .IsResourceType }}
  unknown_data_.bytes = std::move(bytes);
  unknown_data_.handles = std::move(handles);
  {{- else }}
  unknown_data_ = std::move(bytes);
  {{- end }}
  return *this;
}
{{- end }}

void {{ .Name }}::Destroy() {
  switch (tag_) {
  {{- range .Members }}
    case Tag::{{ .TagName }}:
      {{- if .Type.NeedsDtor }}
      {{ .StorageName }}.~decltype({{ .StorageName }})();
      {{- end }}
      break;
  {{- end }}
  {{ if .IsFlexible }}
    case static_cast<fidl_xunion_tag_t>(Tag::Invalid):
      break;
    default:
      unknown_data_.~decltype(unknown_data_)();
      break;
  {{ else }}
    default:
      break;
  {{ end }}
  }
  tag_ = static_cast<fidl_xunion_tag_t>(Tag::Invalid);
}

void {{ .Name }}::EnsureStorageInitialized(::fidl_xunion_tag_t tag) {
  if (tag_ != tag) {
    Destroy();
    tag_ = tag;
    switch (tag_) {
      case static_cast<fidl_xunion_tag_t>(Tag::Invalid):
        break;
      {{- range .Members }}
      case Tag::{{ .TagName }}:
        new (&{{ .StorageName }}) {{ .Type }}();
        break;
      {{- end }}
      default:
      {{- if .IsFlexible }}
        new (&unknown_data_) decltype(unknown_data_);
      {{- end }}
        break;
    }
  }
}
{{- if .IsResourceType }}
{{- PopNamespace }}
#endif  // __Fuchsia__
{{ end }}

{{- end }}

{{- define "UnionTraits" }}
{{- if .IsResourceType }}
#ifdef __Fuchsia__
{{- PushNamespace }}
{{- end }}
template <>
struct IsFidlXUnion<{{ . }}> : public std::true_type {};

template <>
struct CodingTraits<{{ . }}>
    : public EncodableCodingTraits<{{ . }}, {{ .InlineSize }}> {};

template <>
struct CodingTraits<std::unique_ptr<{{ . }}>> {
  static constexpr size_t inline_size_v1_no_ee = {{ .InlineSize }};

  static void Encode(Encoder* encoder, std::unique_ptr<{{ . }}>* value, size_t offset,
                     fit::optional<::fidl::HandleInformation> maybe_handle_info) {
    {{/* TODO(fxbug.dev/7805): Disallow empty xunions (but permit nullable/optional
         xunions). */ -}}

    auto&& p_xunion = *value;
    if (p_xunion) {
      p_xunion->Encode(encoder, offset);
    }
  }

  static void Decode(Decoder* decoder, std::unique_ptr<{{ . }}>* value, size_t offset) {
    fidl_xunion_t* encoded = decoder->GetPtr<fidl_xunion_t>(offset);
    if (encoded->tag == 0) {
      value->reset(nullptr);
      return;
    }

    value->reset(new {{ . }});

    {{ . }}::Decode(decoder, value->get(), offset);
  }
};

inline zx_status_t Clone(const {{ . }}& value,
                         {{ . }}* result) {
  return {{ .Namespace }}::Clone(value, result);
}

template<>
struct Equality<{{ . }}> {
  bool operator()(const {{ . }}& _lhs, const {{ . }}& _rhs) const {
    if (_lhs.Ordinal() != _rhs.Ordinal()) {
      return false;
    }

    {{ with $xunion := . -}}
    switch (_lhs.Ordinal()) {
      case static_cast<fidl_xunion_tag_t>({{ $xunion }}::Tag::Invalid):
        return true;
    {{- range .Members }}
      case {{ $xunion }}::Tag::{{ .TagName }}:
        return ::fidl::Equals(_lhs.{{ .StorageName }}, _rhs.{{ .StorageName }});
      {{- end }}
      {{ if .IsFlexible -}}
      default:
        return ::fidl::Equals(_lhs.unknown_data_, _rhs.unknown_data_);
      {{ else }}
      default:
        return false;
      {{ end -}}
      }
    {{ end -}}
  }
};
{{- if .IsResourceType }}
{{- PopNamespace }}
#endif  // __Fuchsia__
{{ end }}

{{- end }}
`
