// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package summarize

import (
	"fmt"
	"strings"

	"go.fuchsia.dev/fuchsia/tools/fidl/lib/fidlgen"
)

const protocolKind = "protocol"

// addProtocols adds the protocols to the elements list.
func (s *summarizer) addProtocols(protocols []fidlgen.Protocol) {
	for _, p := range protocols {
		for _, m := range p.Methods {
			s.addElement(newMethod(p.Name, m))
		}
		s.addElement(protocol{named: named{name: string(p.Name)}})
	}
}

// protocol represents an element of the protocol type.
type protocol struct {
	named
	notMember
}

// String implements Element.
func (p protocol) String() string {
	return p.Serialize().String()
}

func (p protocol) Serialize() elementStr {
	e := p.named.Serialize()
	e.Kind = protocolKind
	return e
}

// method represents an Element for a protocol method.
type method struct {
	membership isMember
	method     fidlgen.Method
}

// newMethod creates a new protocol method element.
func newMethod(parent fidlgen.EncodedCompoundIdentifier, m fidlgen.Method) method {
	return method{
		membership: newIsMember(parent, m.Name, fidlgen.ProtocolDeclType),
		method:     m,
	}
}

// Name implements Element.
func (m method) Name() string {
	return m.membership.Name()
}

// String implements Element.  It formats a protocol method using a notation
// familiar from FIDL.
func (m method) String() string {
	e := m.Serialize()
	// Method serialization is custom because of different spacing.
	return fmt.Sprintf("%v %v%v", e.Kind, e.Name, e.Decl)
}

// Member implements Element.
func (m method) Member() bool {
	return m.membership.Member()
}

// getTypeSignature returns a string representation of the type signature of
// this method.  E.g. "(int32 a) -> (Foo b)"
func (m method) getTypeSignature() string {
	var parlist []string
	request := getParamList(m.method.HasRequest, m.method.Request)
	if request != "" {
		parlist = append(parlist, request)
	}
	response := getParamList(m.method.HasResponse, m.method.Response)
	if response != "" {
		if request == "" {
			// -> Method(T a)
			parlist = append(parlist, "")
		}
		parlist = append(parlist, "->", response)
	}
	return strings.Join(parlist, " ")
}

func (m method) Serialize() elementStr {
	e := m.membership.Serialize()
	e.Kind = "protocol/member"
	e.Decl = m.getTypeSignature()
	return e
}

// getParamList formats a parameter list, as in Foo(ty1 a, ty2b)
func getParamList(hasParams bool, params []fidlgen.Parameter) string {
	if !hasParams {
		return ""
	}
	var ps []string
	for _, p := range params {
		ps = append(ps, fmt.Sprintf("%v %v", fidlTypeString(p.Type), p.Name))
	}
	return fmt.Sprintf("(%v)", strings.Join(ps, ","))
}
