// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package codegen

const tmplDecoderEncoderHeader = `
{{- define "DecoderEncoderHeader" -}}
// WARNING: This file is machine generated by fidlgen.

#pragma once

// For ::fidl::fuzzing::DecoderEncoder.
#include <lib/fidl/cpp/fuzzing/decoder_encoder.h>
// For ::std::pair.
#include <utility>
// For uint*_t.
#include <stdint.h>
// For zx_handle_info_t and zx_status_t.
#include <zircon/types.h>

namespace fuzzing {

  ::std::vector<::fidl::fuzzing::DecoderEncoder>
{{ range .Library }}{{ . }}_{{ end }}decoder_encoders;

}  // namespace fuzzing
{{ end }}
`
