// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package codegen

const fragmentSyncRequestCallerAllocateTmpl = `
{{- define "CallerBufferParams" -}}
{{- if . -}}
::fidl::BufferSpan _request_buffer, {{ range $index, $param := . -}}
    {{- if $index }}, {{ end -}}{{ $param.Type }} {{ $param.Name }}
  {{- end -}}
{{- end -}}
{{- end }}

{{- define "SyncRequestCallerAllocateMethodArguments" -}}
{{ template "CallerBufferParams" .Request }}{{ if .HasResponse }}{{ if .Request }}, {{ end }}
::fidl::BufferSpan _response_buffer{{ end }}
{{- end }}

{{- define "StaticCallSyncRequestCallerAllocateMethodArguments" -}}
::fidl::UnownedClientEnd<{{ .LLProps.ProtocolName }}> _client_end
{{- if .Request }}, {{ end }}
{{- template "CallerBufferParams" .Request }}
{{- if .HasResponse }}, ::fidl::BufferSpan _response_buffer{{ end }}
{{- end }}

{{- define "SyncRequestCallerAllocateMethodDefinition" }}
{{ .LLProps.ProtocolName }}::UnownedResultOf::{{ .Name }}::{{ .Name }}(
  ::fidl::UnownedClientEnd<{{ .LLProps.ProtocolName }}> _client
  {{- if .Request -}}
  , uint8_t* _request_bytes, uint32_t _request_byte_capacity
  {{- end -}}
  {{- .Request | CommaMessagePrototype }}
  {{- if .HasResponse }}
  , uint8_t* _response_bytes, uint32_t _response_byte_capacity)
    : bytes_(_response_bytes) {
  {{- else }}
  ) {
  {{- end }}
  {{- if .Request -}}
  ::fidl::internal::EncodedMessageTypes<{{ .Name }}Request>::UnownedByte _request(
    _request_bytes, _request_byte_capacity, 0
  {{- else -}}
  ::fidl::internal::EncodedMessageTypes<{{ .Name }}Request>::OwnedByte _request(zx_txid_t(0)
  {{- end -}}
    {{- .Request | CommaParamNames -}});
  {{- if .HasResponse }}
  _request.GetOutgoingMessage().Call<{{ .Name }}Response>(_client, _response_bytes,
                                                          _response_byte_capacity);
  {{- else }}
  _request.GetOutgoingMessage().Write(_client);
  {{- end }}
  status_ = _request.status();
  error_ = _request.error();
}
{{- end }}
`
