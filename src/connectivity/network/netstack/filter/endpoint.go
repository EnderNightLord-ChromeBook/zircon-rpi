// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Filter endpoint implements a LinkEndpoint interface, which can wrap another
// LinkEndpoint.

package filter

import (
	"fmt"
	"sync/atomic"

	"go.fuchsia.dev/fuchsia/src/connectivity/network/netstack/packetbuffer"

	"gvisor.dev/gvisor/pkg/tcpip"
	"gvisor.dev/gvisor/pkg/tcpip/buffer"
	"gvisor.dev/gvisor/pkg/tcpip/link/nested"
	"gvisor.dev/gvisor/pkg/tcpip/stack"
)

var _ stack.LinkEndpoint = (*Endpoint)(nil)
var _ stack.GSOEndpoint = (*Endpoint)(nil)
var _ stack.NetworkDispatcher = (*Endpoint)(nil)

type Endpoint struct {
	filter  *Filter
	enabled uint32
	nested.Endpoint
}

// New creates a new Filter endpoint by wrapping a lower LinkEndpoint.
func NewEndpoint(filter *Filter, lower stack.LinkEndpoint) *Endpoint {
	ep := &Endpoint{
		filter:  filter,
		enabled: 1,
	}
	ep.Endpoint.Init(lower, ep)
	return ep
}

func (e *Endpoint) Enable() {
	atomic.StoreUint32(&e.enabled, 1)
}

func (e *Endpoint) Disable() {
	atomic.StoreUint32(&e.enabled, 0)
}

func (e *Endpoint) IsEnabled() bool {
	return atomic.LoadUint32(&e.enabled) == 1
}

// DeliverNetworkPacket implements stack.NetworkDispatcher.
func (e *Endpoint) DeliverNetworkPacket(dstLinkAddr, srcLinkAddr tcpip.LinkAddress, protocol tcpip.NetworkProtocolNumber, pkt *stack.PacketBuffer) {
	if atomic.LoadUint32(&e.enabled) == 0 {
		e.Endpoint.DeliverNetworkPacket(dstLinkAddr, srcLinkAddr, protocol, pkt)
		return
	}

	// The filter expects the packet's header to be in a single view.
	//
	// Since we are delivering the packet to a NetworkDispatcher, we know only the
	// link header may be set - the rest of the packet will be in Data.
	//
	// Note, the ethernet and netdevice clients always provide a packet buffer
	// that was created with a single view, so we will not incur any allocations
	// or copies below when a packet is received from those clients.
	//
	// TODO(fxbug.dev/50424): Support using a buffer.VectorisedView when parsing packets
	// so we don't need to create a single view here.
	if len(pkt.Data.Views()) == 0 {
		// Expected to receive a packet buffer with at least 1 remaining view in
		// Data.
		return
	}

	if len(pkt.Data.Views()) != 1 {
		// If we have more than 1 view in Data, combine them into a single view.
		// Note, the stack may be interested in the link headers so make sure to
		// not throw it away.
		linkHdrLen := len(pkt.LinkHeader().View())
		pkt = stack.NewPacketBuffer(stack.PacketBufferOptions{
			Data: packetbuffer.ToView(pkt).ToVectorisedView(),
		})
		_, ok := pkt.LinkHeader().Consume(linkHdrLen)
		if !ok {
			panic(fmt.Sprintf("failed to consume %d bytes for the link header", linkHdrLen))
		}
	}

	if e.filter.Run(Incoming, protocol, pkt.Data.Views()[0], buffer.VectorisedView{}) != Pass {
		return
	}

	e.Endpoint.DeliverNetworkPacket(dstLinkAddr, srcLinkAddr, protocol, pkt)
}

// WritePacket implements stack.LinkEndpoint.
func (e *Endpoint) WritePacket(r *stack.Route, gso *stack.GSO, protocol tcpip.NetworkProtocolNumber, pkt *stack.PacketBuffer) *tcpip.Error {
	if atomic.LoadUint32(&e.enabled) == 0 {
		return e.Endpoint.WritePacket(r, gso, protocol, pkt)
	}

	// The filter expects the packet's header to be in the packet buffer's
	// header.
	//
	// TODO(fxbug.dev/50424): Support using a buffer.VectorisedView when parsing packets
	// so we don't need to create a single view here.
	hdr := packetbuffer.ToView(pkt)
	if e.filter.Run(Outgoing, protocol, hdr, buffer.VectorisedView{}) == Pass {
		return e.Endpoint.WritePacket(r, gso, protocol, stack.NewPacketBuffer(stack.PacketBufferOptions{
			ReserveHeaderBytes: int(e.MaxHeaderLength()),
			Data:               hdr.ToVectorisedView(),
		}))
	}

	return nil
}

// WritePackets implements stack.LinkEndpoint.
func (e *Endpoint) WritePackets(r *stack.Route, gso *stack.GSO, pkts stack.PacketBufferList, protocol tcpip.NetworkProtocolNumber) (int, *tcpip.Error) {
	if atomic.LoadUint32(&e.enabled) == 0 {
		return e.Endpoint.WritePackets(r, gso, pkts, protocol)
	}

	var filtered stack.PacketBufferList
	for pkt := pkts.Front(); pkt != nil; pkt = pkt.Next() {
		// The filter expects the packet's header to be in the packet buffer's
		// header.
		//
		// TODO(fxbug.dev/50424): Support using a buffer.VectorisedView when parsing packets
		// so we don't need to create a single view here.
		hdr := packetbuffer.ToView(pkt)
		if e.filter.Run(Outgoing, protocol, hdr, buffer.VectorisedView{}) == Pass {
			filtered.PushBack(stack.NewPacketBuffer(stack.PacketBufferOptions{
				ReserveHeaderBytes: int(e.MaxHeaderLength()),
				Data:               hdr.ToVectorisedView(),
			}))
		}
	}

	return e.Endpoint.WritePackets(r, gso, filtered, protocol)
}
