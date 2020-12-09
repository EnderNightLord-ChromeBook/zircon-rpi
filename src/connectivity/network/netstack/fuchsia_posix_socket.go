// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// +build !build_with_native_toolchain

package netstack

import (
	"context"
	"encoding/binary"
	"fmt"
	"net"
	"reflect"
	"runtime"
	"sort"
	"strings"
	"sync"
	"syscall/zx"
	"syscall/zx/fidl"
	"syscall/zx/zxsocket"
	"syscall/zx/zxwait"
	"time"

	"go.fuchsia.dev/fuchsia/src/connectivity/network/netstack/fidlconv"
	"go.fuchsia.dev/fuchsia/src/lib/component"
	syslog "go.fuchsia.dev/fuchsia/src/lib/syslog/go"

	"fidl/fuchsia/io"
	fidlnet "fidl/fuchsia/net"
	"fidl/fuchsia/posix"
	"fidl/fuchsia/posix/socket"

	"gvisor.dev/gvisor/pkg/tcpip"
	"gvisor.dev/gvisor/pkg/tcpip/buffer"
	"gvisor.dev/gvisor/pkg/tcpip/header"
	"gvisor.dev/gvisor/pkg/tcpip/network/ipv4"
	"gvisor.dev/gvisor/pkg/tcpip/network/ipv6"
	"gvisor.dev/gvisor/pkg/tcpip/stack"
	"gvisor.dev/gvisor/pkg/tcpip/transport/icmp"
	"gvisor.dev/gvisor/pkg/tcpip/transport/tcp"
	"gvisor.dev/gvisor/pkg/tcpip/transport/udp"
	"gvisor.dev/gvisor/pkg/waiter"
)

// TODO(fxbug.dev/44347) We shouldn't need any of this includes after we remove
// C structs from the wire.

/*
#cgo CFLAGS: -D_GNU_SOURCE
#cgo CFLAGS: -I${SRCDIR}/../../../../zircon/system/ulib/zxs/include
#cgo CFLAGS: -I${SRCDIR}/../../../../zircon/third_party/ulib/musl/include
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <netinet/in.h>
*/
import "C"

// endpoint is the base structure that models all network sockets.
type endpoint struct {
	// TODO(fxbug.dev/37419): Remove TransitionalBase after methods landed.
	*io.NodeWithCtxTransitionalBase

	wq *waiter.Queue
	ep tcpip.Endpoint

	mu struct {
		sync.Mutex
		refcount         uint32
		sockOptTimestamp bool
	}

	transProto tcpip.TransportProtocolNumber
	netProto   tcpip.NetworkProtocolNumber

	ns *Netstack

	hardErrorMu struct {
		sync.Mutex
		err *tcpip.Error
	}
}

func (ep *endpoint) incRef() {
	ep.mu.Lock()
	ep.mu.refcount++
	ep.mu.Unlock()
}

func (ep *endpoint) decRef() bool {
	ep.mu.Lock()
	doubleClose := ep.mu.refcount == 0
	ep.mu.refcount--
	doClose := ep.mu.refcount == 0
	ep.mu.Unlock()
	if doubleClose {
		panic(fmt.Sprintf("%p: double close", ep))
	}
	return doClose
}

// storeAndRetrieveHardErrorLocked evaluates if the input error is a "hard
// error" (one which puts the endpoint in an unrecoverable error state) and
// stores it. Returns the pre-existing hard error if it was already set or the
// new value if changed.
//
// Must be called with hardErrorMu locked.
func (ep *endpoint) storeAndRetrieveHardErrorLocked(err *tcpip.Error) *tcpip.Error {
	if ep.hardErrorMu.err == nil {
		switch err {
		case tcpip.ErrConnectionAborted, tcpip.ErrConnectionReset,
			tcpip.ErrNetworkUnreachable, tcpip.ErrNoRoute, tcpip.ErrTimeout,
			tcpip.ErrConnectionRefused:
			ep.hardErrorMu.err = err
		}
	}
	return ep.hardErrorMu.err
}

func (ep *endpoint) Sync(fidl.Context) (int32, error) {
	syslog.VLogTf(syslog.DebugVerbosity, "Sync", "%p", ep)

	return 0, &zx.Error{Status: zx.ErrNotSupported, Text: fmt.Sprintf("%T", ep)}
}

func (ep *endpoint) GetAttr(fidl.Context) (int32, io.NodeAttributes, error) {
	syslog.VLogTf(syslog.DebugVerbosity, "GetAttr", "%p", ep)

	return 0, io.NodeAttributes{}, &zx.Error{Status: zx.ErrNotSupported, Text: fmt.Sprintf("%T", ep)}
}

func (ep *endpoint) SetAttr(_ fidl.Context, flags uint32, attributes io.NodeAttributes) (int32, error) {
	syslog.VLogTf(syslog.DebugVerbosity, "SetAttr", "%p", ep)

	return 0, &zx.Error{Status: zx.ErrNotSupported, Text: fmt.Sprintf("%T", ep)}
}

func (ep *endpoint) Bind(_ fidl.Context, sockaddr fidlnet.SocketAddress) (socket.BaseSocketBindResult, error) {
	addr, err := toTCPIPFullAddress(sockaddr)
	if err != nil {
		return socket.BaseSocketBindResultWithErr(tcpipErrorToCode(tcpip.ErrBadAddress)), nil
	}
	if err := ep.ep.Bind(addr); err != nil {
		return socket.BaseSocketBindResultWithErr(tcpipErrorToCode(err)), nil
	}

	{
		localAddr, err := ep.ep.GetLocalAddress()
		if err != nil {
			panic(err)
		}
		syslog.VLogTf(syslog.DebugVerbosity, "bind", "%p: local=%+v", ep, localAddr)
	}

	return socket.BaseSocketBindResultWithResponse(socket.BaseSocketBindResponse{}), nil
}

func (ep *endpoint) Connect(_ fidl.Context, address fidlnet.SocketAddress) (socket.BaseSocketConnectResult, error) {
	addr, err := toTCPIPFullAddress(address)
	if err != nil {
		return socket.BaseSocketConnectResultWithErr(tcpipErrorToCode(tcpip.ErrBadAddress)), nil
	}
	// NB: We can't just compare the length to zero because that would
	// mishandle the IPv6-mapped IPv4 unspecified address.
	disconnect := addr.Port == 0 && (len(addr.Addr) == 0 || net.IP(addr.Addr).IsUnspecified())
	if disconnect {
		if err := ep.ep.Disconnect(); err != nil {
			return socket.BaseSocketConnectResultWithErr(tcpipErrorToCode(err)), nil
		}
	} else {
		if l := len(addr.Addr); l > 0 {
			if ep.netProto == ipv4.ProtocolNumber && l != header.IPv4AddressSize {
				syslog.VLogTf(syslog.DebugVerbosity, "connect", "%p: unsupported address %s", ep, addr.Addr)
				return socket.BaseSocketConnectResultWithErr(tcpipErrorToCode(tcpip.ErrAddressFamilyNotSupported)), nil
			}
		}

		// Acquire hard error lock across ep calls to avoid races and store the
		// hard error deterministically.
		ep.hardErrorMu.Lock()
		err := ep.ep.Connect(addr)
		hardError := ep.storeAndRetrieveHardErrorLocked(err)
		ep.hardErrorMu.Unlock()
		if err != nil {
			switch err {
			case tcpip.ErrConnectStarted:
				localAddr, err := ep.ep.GetLocalAddress()
				if err != nil {
					panic(err)
				}
				syslog.VLogTf(syslog.DebugVerbosity, "connect", "%p: started, local=%+v, addr=%+v", ep, localAddr, addr)
			// For TCP endpoints, gVisor Connect() returns this error when the endpoint
			// is in an error state and the hard error state has already been read from the
			// endpoint via other APIs. Apply the saved hard error state here.
			case tcpip.ErrConnectionAborted:
				if hardError != nil {
					err = hardError
				}
			}
			return socket.BaseSocketConnectResultWithErr(tcpipErrorToCode(err)), nil
		}
	}

	{
		localAddr, err := ep.ep.GetLocalAddress()
		if err != nil {
			panic(err)
		}

		if disconnect {
			syslog.VLogTf(syslog.DebugVerbosity, "connect", "%p: local=%+v, remote=disconnected", ep, localAddr)
		} else {
			remoteAddr, err := ep.ep.GetRemoteAddress()
			if err != nil {
				panic(err)
			}
			syslog.VLogTf(syslog.DebugVerbosity, "connect", "%p: local=%+v, remote=%+v", ep, localAddr, remoteAddr)
		}
	}

	return socket.BaseSocketConnectResultWithResponse(socket.BaseSocketConnectResponse{}), nil
}

func (ep *endpoint) Disconnect(_ fidl.Context) (socket.BaseSocketDisconnectResult, error) {
	if err := ep.ep.Disconnect(); err != nil {
		return socket.BaseSocketDisconnectResultWithErr(tcpipErrorToCode(err)), nil
	}
	return socket.BaseSocketDisconnectResultWithResponse(socket.BaseSocketDisconnectResponse{}), nil
}

func (ep *endpoint) GetSockName(fidl.Context) (socket.BaseSocketGetSockNameResult, error) {
	addr, err := ep.ep.GetLocalAddress()
	if err != nil {
		return socket.BaseSocketGetSockNameResultWithErr(tcpipErrorToCode(err)), nil
	}
	return socket.BaseSocketGetSockNameResultWithResponse(socket.BaseSocketGetSockNameResponse{
		Addr: toNetSocketAddress(ep.netProto, addr),
	}), nil
}

func (ep *endpoint) GetPeerName(fidl.Context) (socket.BaseSocketGetPeerNameResult, error) {
	addr, err := ep.ep.GetRemoteAddress()
	if err != nil {
		return socket.BaseSocketGetPeerNameResultWithErr(tcpipErrorToCode(err)), nil
	}
	return socket.BaseSocketGetPeerNameResultWithResponse(socket.BaseSocketGetPeerNameResponse{
		Addr: toNetSocketAddress(ep.netProto, addr),
	}), nil
}

func (ep *endpoint) SetSockOpt(_ fidl.Context, level, optName int16, optVal []uint8) (socket.BaseSocketSetSockOptResult, error) {
	if level == C.SOL_SOCKET && optName == C.SO_TIMESTAMP {
		if len(optVal) < sizeOfInt32 {
			return socket.BaseSocketSetSockOptResultWithErr(tcpipErrorToCode(tcpip.ErrInvalidOptionValue)), nil
		}

		v := binary.LittleEndian.Uint32(optVal)
		ep.mu.Lock()
		ep.mu.sockOptTimestamp = v != 0
		ep.mu.Unlock()
	} else {
		if err := SetSockOpt(ep.ep, ep.ns, level, optName, optVal); err != nil {
			return socket.BaseSocketSetSockOptResultWithErr(tcpipErrorToCode(err)), nil
		}
	}
	syslog.VLogTf(syslog.DebugVerbosity, "setsockopt", "%p: level=%d, optName=%d, optVal[%d]=%v", ep, level, optName, len(optVal), optVal)

	return socket.BaseSocketSetSockOptResultWithResponse(socket.BaseSocketSetSockOptResponse{}), nil
}

func (ep *endpoint) GetSockOpt(_ fidl.Context, level, optName int16) (socket.BaseSocketGetSockOptResult, error) {
	var val interface{}
	if level == C.SOL_SOCKET && optName == C.SO_TIMESTAMP {
		ep.mu.Lock()
		if ep.mu.sockOptTimestamp {
			val = int32(1)
		} else {
			val = int32(0)
		}
		ep.mu.Unlock()
	} else {
		var err *tcpip.Error
		val, err = GetSockOpt(ep.ep, ep.ns, ep.netProto, ep.transProto, level, optName)
		if err != nil {
			return socket.BaseSocketGetSockOptResultWithErr(tcpipErrorToCode(err)), nil
		}
	}
	if val, ok := val.([]byte); ok {
		return socket.BaseSocketGetSockOptResultWithResponse(socket.BaseSocketGetSockOptResponse{
			Optval: val,
		}), nil
	}
	b := make([]byte, reflect.TypeOf(val).Size())
	n := copyAsBytes(b, val)
	if n < len(b) {
		panic(fmt.Sprintf("short %T: %d/%d", val, n, len(b)))
	}
	syslog.VLogTf(syslog.DebugVerbosity, "getsockopt", "%p: level=%d, optName=%d, optVal[%d]=%v", ep, level, optName, len(b), b)

	return socket.BaseSocketGetSockOptResultWithResponse(socket.BaseSocketGetSockOptResponse{
		Optval: b,
	}), nil
}

type boolWithMutex struct {
	mu struct {
		sync.Mutex
		asserted bool
	}
}

// endpointWithSocket implements a network socket that uses a zircon socket for
// its data plane. This structure creates a pair of goroutines which are
// responsible for moving data and signals between the underlying
// tcpip.Endpoint and the zircon socket.
type endpointWithSocket struct {
	endpoint

	local, peer zx.Socket

	incoming boolWithMutex

	// These channels enable coordination of orderly shutdown of loops, handles,
	// and endpoints. See the comment on `close` for more information.
	//
	// Notes:
	//
	//  - closing is signaled iff close has been called.
	//
	//  - loop{Read,Write}Done are signaled iff loop{Read,Write} have
	//    exited, respectively.
	//
	//  - loopPollDone is signaled when the poller goroutine exits.
	closing, loopReadDone, loopWriteDone, loopPollDone chan struct{}

	// This is used to make sure that endpoint.close only cleans up its
	// resources once - the first time it was closed.
	closeOnce sync.Once

	// Used to unblock waiting to write when SO_LINGER is enabled.
	linger chan struct{}

	// entry is used to register callback for error and closing events.
	entry waiter.Entry
}

func newEndpointWithSocket(ep tcpip.Endpoint, wq *waiter.Queue, transProto tcpip.TransportProtocolNumber, netProto tcpip.NetworkProtocolNumber, ns *Netstack) (*endpointWithSocket, error) {
	var flags uint32
	if transProto == tcp.ProtocolNumber {
		flags |= zx.SocketStream
	} else {
		flags |= zx.SocketDatagram
	}
	localS, peerS, err := zx.NewSocket(flags)
	if err != nil {
		return nil, err
	}

	eps := &endpointWithSocket{
		endpoint: endpoint{
			ep:         ep,
			wq:         wq,
			transProto: transProto,
			netProto:   netProto,
			ns:         ns,
		},
		local:         localS,
		peer:          peerS,
		loopReadDone:  make(chan struct{}),
		loopWriteDone: make(chan struct{}),
		loopPollDone:  make(chan struct{}),
		closing:       make(chan struct{}),
		linger:        make(chan struct{}),
	}

	// Add the endpoint before registering for an EventHUp callback and starting
	// the loop{read,Write} go-routines. We remove the endpoint from the map on
	// EventHUp which can be trigerred soon-after the callback registration or
	// starting of the loop{Read,Write}.
	ns.onAddEndpoint(zx.Handle(localS), ep)

	// Register a callback for error and closing events from gVisor to
	// trigger a close of the endpoint.
	eps.entry.Callback = callback(func(*waiter.Entry) {
		// Run this in a separate goroutine to avoid deadlock.
		//
		// The waiter.Queue lock is held by the caller of this callback.
		// close() blocks on completions of `loop*`, which
		// depend on acquiring waiter.Queue lock to unregister events.
		go eps.close()
	})
	eps.wq.EventRegister(&eps.entry, waiter.EventHUp)

	go func() {
		defer close(eps.loopPollDone)

		sigs := zx.Signals(zx.SignalSocketWriteDisabled | localSignalClosing)

		for {
			obs, err := zxwait.Wait(zx.Handle(eps.local), sigs, zx.TimensecInfinite)
			if err != nil {
				panic(err)
			}

			if obs&sigs&zx.SignalSocketWriteDisabled != 0 {
				sigs ^= zx.SignalSocketWriteDisabled
				if err := eps.ep.Shutdown(tcpip.ShutdownRead); err != nil && err != tcpip.ErrNotConnected {
					panic(err)
				}
			}

			if obs&localSignalClosing != 0 {
				// We're shutting down.
				return
			}
		}
	}()

	{
		// Synchronize with loopRead to ensure that:
		//
		// - we observe all incoming event notifications; such notifications can
		// begin to arrive as soon as this function returns, so we must register
		// before returning.
		//
		// - we correctly initialize the connected state before allowing it to be
		// observed by the peer; the state is observable as soon as this function
		// returns, so we must wait to be notified before returning.
		initCh := make(chan struct{})
		go func() {
			inEntry, inCh := waiter.NewChannelEntry(nil)
			eps.wq.EventRegister(&inEntry, waiter.EventIn)
			defer eps.wq.EventUnregister(&inEntry)

			eps.loopRead(inCh, initCh)
		}()
		<-initCh
	}

	go eps.loopWrite()

	return eps, nil
}

type endpointWithEvent struct {
	endpoint

	mu struct {
		sync.Mutex
		readView buffer.View
		sender   tcpip.FullAddress
	}

	local, peer zx.Handle

	incoming boolWithMutex

	entry waiter.Entry
}

func (epe *endpointWithEvent) Describe(fidl.Context) (io.NodeInfo, error) {
	var info io.NodeInfo
	event, err := epe.peer.Duplicate(zx.RightsBasic)
	syslog.VLogTf(syslog.DebugVerbosity, "Describe", "%p: err=%v", epe, err)
	if err != nil {
		return info, err
	}
	info.SetDatagramSocket(io.DatagramSocket{Event: event})
	return info, nil
}

func (epe *endpointWithEvent) Shutdown(_ fidl.Context, how socket.ShutdownMode) (socket.DatagramSocketShutdownResult, error) {
	var signals zx.Signals
	var flags tcpip.ShutdownFlags

	if how&socket.ShutdownModeRead != 0 {
		signals |= zxsocket.SignalShutdownRead
		flags |= tcpip.ShutdownRead
	}
	if how&socket.ShutdownModeWrite != 0 {
		signals |= zxsocket.SignalShutdownWrite
		flags |= tcpip.ShutdownWrite
	}
	if flags == 0 {
		return socket.DatagramSocketShutdownResultWithErr(C.EINVAL), nil
	}
	if err := epe.ep.Shutdown(flags); err != nil {
		return socket.DatagramSocketShutdownResultWithErr(tcpipErrorToCode(err)), nil
	}
	if flags&tcpip.ShutdownRead != 0 {
		epe.wq.EventUnregister(&epe.entry)
	}
	if err := epe.local.SignalPeer(0, signals); err != nil {
		return socket.DatagramSocketShutdownResult{}, err
	}
	return socket.DatagramSocketShutdownResultWithResponse(socket.DatagramSocketShutdownResponse{}), nil
}

const localSignalClosing = zx.SignalUser1

// close destroys the endpoint and releases associated resources.
//
// When called, close signals loopRead and loopWrite (via closing and
// local) to exit, and then blocks until its arguments are signaled. close
// is typically called with loop{Read,Write}Done.
//
// Note, calling close on an endpoint that has already been closed is safe as
// the cleanup work will only be done once.
func (eps *endpointWithSocket) close() {
	eps.closeOnce.Do(func() {
		// Interrupt waits on notification channels. Notification reads
		// are always combined with closing in a select statement.
		close(eps.closing)

		// Interrupt waits on endpoint.local. Handle waits always
		// include localSignalClosing.
		if err := eps.local.Handle().Signal(0, localSignalClosing); err != nil {
			panic(err)
		}

		// The interruptions above cause our loops to exit. Wait until
		// they do before releasing resources they may be using.
		for _, ch := range []<-chan struct{}{
			eps.loopReadDone,
			eps.loopWriteDone,
			eps.loopPollDone,
		} {
			<-ch
		}

		// Copy the handle before closing below; (*zx.Handle).Close sets the
		// receiver to zx.HandleInvalid.
		key := zx.Handle(eps.local)

		if err := eps.local.Close(); err != nil {
			panic(err)
		}

		eps.wq.EventUnregister(&eps.entry)

		eps.endpoint.ns.onRemoveEndpoint(key)

		eps.ep.Close()

		syslog.VLogTf(syslog.DebugVerbosity, "close", "%p", eps)
	})
}

func (eps *endpointWithSocket) Listen(_ fidl.Context, backlog int16) (socket.StreamSocketListenResult, error) {
	if backlog < 0 {
		backlog = 0
	}
	if err := eps.ep.Listen(int(backlog)); err != nil {
		return socket.StreamSocketListenResultWithErr(tcpipErrorToCode(err)), nil
	}

	syslog.VLogTf(syslog.DebugVerbosity, "listen", "%p: backlog=%d", eps, backlog)

	return socket.StreamSocketListenResultWithResponse(socket.StreamSocketListenResponse{}), nil
}

func (eps *endpointWithSocket) Accept(wantAddr bool) (posix.Errno, *tcpip.FullAddress, *endpointWithSocket, error) {
	var addr *tcpip.FullAddress
	if wantAddr {
		addr = new(tcpip.FullAddress)
	}
	ep, wq, err := eps.endpoint.ep.Accept(addr)
	if err != nil {
		return tcpipErrorToCode(err), nil, nil, nil
	}
	{
		var err error
		// We lock here to ensure that no incoming connection changes readiness
		// while we clear the signal.
		eps.incoming.mu.Lock()
		if eps.incoming.mu.asserted && eps.endpoint.ep.Readiness(waiter.EventIn) == 0 {
			err = eps.local.Handle().SignalPeer(zxsocket.SignalIncoming, 0)
			eps.incoming.mu.asserted = false
		}
		eps.incoming.mu.Unlock()
		if err != nil {
			ep.Close()
			return 0, nil, nil, err
		}
	}

	if localAddr, err := ep.GetLocalAddress(); err == tcpip.ErrNotConnected {
		// This should never happen as of writing as GetLocalAddress
		// does not actually return any errors. However, we handle
		// the tcpip.ErrNotConnected case now for the same reasons
		// as mentioned below for the ep.GetRemoteAddress case.
		syslog.VLogTf(syslog.DebugVerbosity, "accept", "%p: disconnected", eps)
	} else if err != nil {
		panic(err)
	} else {
		// GetRemoteAddress returns a tcpip.ErrNotConnected error if ep is no
		// longer connected. This can happen if the endpoint was closed after
		// the call to Accept returned, but before this point. A scenario this
		// was actually witnessed was when a TCP RST was received after the call
		// to Accept returned, but before this point. If GetRemoteAddress
		// returns other (unexpected) errors, panic.
		if remoteAddr, err := ep.GetRemoteAddress(); err == tcpip.ErrNotConnected {
			syslog.VLogTf(syslog.DebugVerbosity, "accept", "%p: local=%+v, disconnected", eps, localAddr)
		} else if err != nil {
			panic(err)
		} else {
			syslog.VLogTf(syslog.DebugVerbosity, "accept", "%p: local=%+v, remote=%+v", eps, localAddr, remoteAddr)
		}
	}
	{
		ep, err := newEndpointWithSocket(ep, wq, eps.transProto, eps.netProto, eps.endpoint.ns)
		return 0, addr, ep, err
	}
}

// loopWrite shuttles signals and data from the zircon socket to the tcpip.Endpoint.
func (eps *endpointWithSocket) loopWrite() {
	triggerClose := false
	defer func() {
		close(eps.loopWriteDone)
		if triggerClose {
			eps.close()
		}
	}()

	const sigs = zx.SignalSocketReadable | zx.SignalSocketPeerWriteDisabled | localSignalClosing

	waitEntry, notifyCh := waiter.NewChannelEntry(nil)
	eps.wq.EventRegister(&waitEntry, waiter.EventOut)
	defer eps.wq.EventUnregister(&waitEntry)

	for {
		// TODO: obviously allocating for each read is silly. A quick hack we can
		// do is store these in a ring buffer, as the lifecycle of this buffer.View
		// starts here, and ends in nearby code we control in link.go.
		v := make([]byte, 0, 2048)
		n, err := eps.local.Read(v[:cap(v)], 0)
		if err != nil {
			if err, ok := err.(*zx.Error); ok {
				switch err.Status {
				case zx.ErrBadState:
					// Reading has been disabled for this socket endpoint.
					if err := eps.ep.Shutdown(tcpip.ShutdownWrite); err != nil && err != tcpip.ErrNotConnected {
						panic(err)
					}
					return
				case zx.ErrShouldWait:
					obs, err := zxwait.Wait(zx.Handle(eps.local), sigs, zx.TimensecInfinite)
					if err != nil {
						panic(err)
					}
					switch {
					case obs&zx.SignalSocketPeerWriteDisabled != 0:
						// The next Read will return zx.ErrBadState.
						continue
					case obs&zx.SignalSocketReadable != 0:
						// The client might have written some data into the socket. Always
						// continue to the loop below and try to read even if the signals
						// show the client has closed the socket.
						continue
					case obs&localSignalClosing != 0:
						// We're shutting down.
						return
					}
				}
			}
			panic(err)
		}
		v = v[:n]

		for {
			// Acquire hard error lock across ep calls to avoid races and store the
			// hard error deterministically.
			eps.hardErrorMu.Lock()
			n, resCh, err := eps.ep.Write(tcpip.SlicePayload(v), tcpip.WriteOptions{})
			hardError := eps.storeAndRetrieveHardErrorLocked(err)
			eps.hardErrorMu.Unlock()

			if resCh != nil {
				if err != tcpip.ErrNoLinkAddress {
					panic(fmt.Sprintf("err=%v inconsistent with presence of resCh", err))
				}
				if eps.transProto == tcp.ProtocolNumber {
					panic(fmt.Sprintf("TCP link address resolutions happen on connect; saw %d/%d", n, len(v)))
				}
				<-resCh
				continue
			}
			// TODO(fxb.dev/35006): Handle all transport write errors.
			switch err {
			case nil:
				if eps.transProto != tcp.ProtocolNumber {
					if n < int64(len(v)) {
						panic(fmt.Sprintf("UDP disallows short writes; saw: %d/%d", n, len(v)))
					}
				}
				v = v[n:]
				if len(v) != 0 {
					continue
				}
			case tcpip.ErrWouldBlock:
				if eps.transProto != tcp.ProtocolNumber {
					panic(fmt.Sprintf("UDP writes are nonblocking; saw %d/%d", n, len(v)))
				}
				// NB: we can't select on closing here because the client may have
				// written some data into the buffer and then immediately closed the
				// socket.
				//
				// We must wait until the linger timeout.
				select {
				case <-eps.linger:
					return
				case <-notifyCh:
					continue
				}
			case tcpip.ErrClosedForSend:
				// Closed for send can be issued when the endpoint is in an error state,
				// which is encoded by the presence of a hard error having been
				// observed.
				// To avoid racing signals with the closing caused by a hard error,
				// we won't signal here if a hard error is already observed.
				if hardError == nil {
					if err := eps.local.Shutdown(zx.SocketShutdownRead); err != nil {
						panic(err)
					}
				}
				return
			case tcpip.ErrConnectionAborted, tcpip.ErrConnectionReset, tcpip.ErrNetworkUnreachable, tcpip.ErrNoRoute:
				triggerClose = true
				return
			case tcpip.ErrTimeout:
				// The maximum duration of missing ACKs was reached, or the maximum
				// number of unacknowledged keepalives was reached.
				triggerClose = true
				return
			default:
				syslog.Errorf("TCP Endpoint.Write(): %s", err)
			}
			break
		}
	}
}

// loopRead shuttles signals and data from the tcpip.Endpoint to the zircon socket.
func (eps *endpointWithSocket) loopRead(inCh <-chan struct{}, initCh chan<- struct{}) {
	triggerClose := false
	defer func() {
		close(eps.loopReadDone)
		if triggerClose {
			eps.close()
		}
	}()

	initDone := func() {
		if initCh != nil {
			close(initCh)
			initCh = nil
		}
	}

	const sigs = zx.SignalSocketWritable | zx.SignalSocketWriteDisabled | localSignalClosing

	outEntry, outCh := waiter.NewChannelEntry(nil)
	connected := eps.transProto != tcp.ProtocolNumber
	if !connected {
		eps.wq.EventRegister(&outEntry, waiter.EventOut)
		defer func() {
			if !connected {
				// If connected became true then we must have already unregistered
				// below. We must never unregister the same entry twice because that
				// can corrupt the waiter queue.
				eps.wq.EventUnregister(&outEntry)
			}
		}()
	}

	var sender tcpip.FullAddress
	for {
		var v []byte

		for {
			var err *tcpip.Error
			// Acquire hard error lock across ep calls to avoid races and store the
			// hard error deterministically.
			eps.hardErrorMu.Lock()
			v, _, err = eps.ep.Read(&sender)
			hardError := eps.storeAndRetrieveHardErrorLocked(err)
			eps.hardErrorMu.Unlock()
			if err == tcpip.ErrNotConnected {
				if connected {
					panic(fmt.Sprintf("connected endpoint returned %s", err))
				}
				// We're not connected; unblock the caller before waiting for incoming packets.
				initDone()
				select {
				case <-eps.closing:
					// We're shutting down.
					return
				case <-inCh:
					// We got an incoming connection; we must be a listening socket.
					// Because we are a listening socket, we don't expect anymore
					// outbound events so there's no harm in letting outEntry remain
					// registered until the end of the function.
					var err error
					eps.incoming.mu.Lock()
					if !eps.incoming.mu.asserted && eps.endpoint.ep.Readiness(waiter.EventIn) != 0 {
						err = eps.local.Handle().SignalPeer(0, zxsocket.SignalIncoming)
						eps.incoming.mu.asserted = true
					}
					eps.incoming.mu.Unlock()
					if err != nil {
						panic(err)
					}
					continue
				case <-outCh:
					// We became connected; the next Read will reflect this.
					continue
				}
			} else if !connected {
				var signals zx.Signals = zxsocket.SignalOutgoing
				switch err {
				case nil, tcpip.ErrWouldBlock, tcpip.ErrClosedForReceive:
					connected = true
					eps.wq.EventUnregister(&outEntry)

					signals |= zxsocket.SignalConnected
				}

				if err := eps.local.Handle().SignalPeer(0, signals); err != nil {
					panic(err)
				}
			}
			// Either we're connected or not; unblock the caller.
			initDone()
			// TODO(fxb.dev/35006): Handle all transport read errors.
			switch err {
			case nil:
			case tcpip.ErrNoLinkAddress:
				// TODO(tamird/iyerm): revisit this assertion when
				// https://github.com/google/gvisor/issues/751 is fixed.
				if connected {
					panic(fmt.Sprintf("Endpoint.Read() = %s on a connected socket should never happen", err))
				}
				// At the time of writing, this error is only possible when link
				// address resolution fails during an outbound TCP connection attempt.
				// This happens via the following call hierarchy:
				//
				//  (*tcp.endpoint).protocolMainLoop
				//    (*tcp.handshake).execute
				//      (*tcp.handshake).resolveRoute
				//        (*stack.Route).Resolve
				//          (*stack.Stack).GetLinkAddress
				//            (*stack.linkAddrCache).get
				//
				// This is equivalent to the connection having been refused.
				fallthrough
			case tcpip.ErrTimeout:
				// At the time of writing, this error indicates that a TCP connection
				// has failed. This can occur during the TCP handshake if the peer
				// fails to respond to a SYN within 60 seconds, or if the retransmit
				// logic gives up after 60 seconds of missing ACKs from the peer, or if
				// the maximum number of unacknowledged keepalives is reached.
				if connected {
					// The connection was alive but now is dead - this is equivalent to
					// having received a TCP RST.
					triggerClose = true
					return
				}
				// The connection was never created. This is equivalent to the
				// connection having been refused.
				fallthrough
			case tcpip.ErrConnectionRefused:
				// Linux allows sockets with connection errors to be reused. If the
				// client calls connect() again (and the underlying Endpoint correctly
				// permits the attempt), we need to wait for an outbound event again.
				select {
				case <-outCh:
					continue
				case <-eps.closing:
					// We're shutting down.
					return
				}
			case tcpip.ErrWouldBlock:
				select {
				case <-inCh:
					continue
				case <-eps.closing:
					// We're shutting down.
					return
				}
			case tcpip.ErrClosedForReceive:
				// Closed for receive can be issued when the endpoint is in an error
				// state, which is encoded by the presence of a hard error having been
				// observed.
				// To avoid racing signals with the closing caused by a hard error,
				// we won't signal here if a hard error is already observed.
				if hardError == nil {
					if err := eps.local.Shutdown(zx.SocketShutdownWrite); err != nil {
						panic(err)
					}
				}
				return
			case tcpip.ErrConnectionAborted, tcpip.ErrConnectionReset, tcpip.ErrNetworkUnreachable, tcpip.ErrNoRoute:
				triggerClose = true
				return
			default:
				syslog.Errorf("Endpoint.Read(): %s", err)
			}
			break
		}

		for {
			n, err := eps.local.Write(v, 0)
			if err != nil {
				if err, ok := err.(*zx.Error); ok {
					switch err.Status {
					case zx.ErrBadState:
						// Writing has been disabled for this socket endpoint.
						if err := eps.ep.Shutdown(tcpip.ShutdownRead); err != nil {
							// An ErrNotConnected while connected is expected if there
							// is pending data to be read and the connection has been
							// reset by the other end of the endpoint. The endpoint will
							// allow the pending data to be read without error but will
							// return ErrNotConnected if Shutdown is called. Otherwise
							// this is unexpected, panic.
							if !(connected && err == tcpip.ErrNotConnected) {
								panic(err)
							}
							syslog.InfoTf("loopRead", "%p: client shutdown a closed endpoint with %d bytes pending data; ep info: %+v", eps, len(v), eps.endpoint.ep.Info())
						}
						return
					case zx.ErrShouldWait:
						obs, err := zxwait.Wait(zx.Handle(eps.local), sigs, zx.TimensecInfinite)
						if err != nil {
							panic(err)
						}
						switch {
						case obs&zx.SignalSocketWriteDisabled != 0:
							// The next Write will return zx.ErrBadState.
							continue
						case obs&zx.SignalSocketWritable != 0:
							continue
						case obs&localSignalClosing != 0:
							// We're shutting down.
							return
						}
					}
				}
				panic(err)
			}
			if eps.transProto != tcp.ProtocolNumber {
				if n < len(v) {
					panic(fmt.Sprintf("UDP disallows short writes; saw: %d/%d", n, len(v)))
				}
			}
			v = v[n:]
			if len(v) == 0 {
				break
			}
			eps.ep.ModerateRecvBuf(n)
		}
	}
}

type datagramSocketImpl struct {
	*endpointWithEvent

	cancel context.CancelFunc
}

var _ socket.DatagramSocketWithCtx = (*datagramSocketImpl)(nil)

func (s *datagramSocketImpl) close() {
	if s.endpoint.decRef() {
		s.wq.EventUnregister(&s.entry)

		// Copy the handle before closing below; (*zx.Handle).Close sets the
		// receiver to zx.HandleInvalid.
		key := s.local

		if err := s.local.Close(); err != nil {
			panic(fmt.Sprintf("local.Close() = %s", err))
		}

		if err := s.peer.Close(); err != nil {
			panic(fmt.Sprintf("peer.Close() = %s", err))
		}

		s.ns.onRemoveEndpoint(key)

		s.ep.Close()

		syslog.VLogTf(syslog.DebugVerbosity, "close", "%p", s.endpointWithEvent)
	}
	s.cancel()
}

func (s *datagramSocketImpl) Close(fidl.Context) (int32, error) {
	syslog.VLogTf(syslog.DebugVerbosity, "Close", "%p", s.endpointWithEvent)
	s.close()
	return int32(zx.ErrOk), nil
}

func (s *datagramSocketImpl) addConnection(_ fidl.Context, object io.NodeWithCtxInterfaceRequest) {
	{
		sCopy := *s
		s := &sCopy

		s.ns.stats.SocketCount.Increment()
		s.endpoint.incRef()
		go func() {
			defer s.ns.stats.SocketCount.Decrement()

			ctx, cancel := context.WithCancel(context.Background())
			s.cancel = cancel
			defer func() {
				// Avoid double close when the peer calls Close and then hangs up.
				if ctx.Err() == nil {
					s.close()
				}
			}()

			stub := socket.DatagramSocketWithCtxStub{Impl: s}
			component.ServeExclusive(ctx, &stub, object.Channel, func(err error) {
				// NB: this protocol is not discoverable, so the bindings do not include its name.
				_ = syslog.WarnTf("fuchsia.posix.socket.DatagramSocket", "%s", err)
			})
		}()
	}
}

func (s *datagramSocketImpl) Clone(ctx fidl.Context, flags uint32, object io.NodeWithCtxInterfaceRequest) error {
	s.addConnection(ctx, object)

	syslog.VLogTf(syslog.DebugVerbosity, "Clone", "%p: flags=%b", s.endpointWithEvent, flags)

	return nil
}

func (s *datagramSocketImpl) RecvMsg(_ fidl.Context, wantAddr bool, dataLen uint32, wantControl bool, flags socket.RecvMsgFlags) (socket.DatagramSocketRecvMsgResult, error) {
	s.mu.Lock()
	var err *tcpip.Error
	if len(s.mu.readView) == 0 {
		// TODO(fxbug.dev/21106): do something with control messages.
		s.mu.readView, _, err = s.ep.Read(&s.mu.sender)
	}
	v, sender := s.mu.readView, s.mu.sender
	if flags&socket.RecvMsgFlagsPeek == 0 {
		s.mu.readView = nil
		s.mu.sender = tcpip.FullAddress{}
	}
	s.mu.Unlock()
	if err != nil {
		return socket.DatagramSocketRecvMsgResultWithErr(tcpipErrorToCode(err)), nil
	}
	{
		var err error
		// We lock here to ensure that no incoming data changes readiness while we
		// clear the signal.
		s.incoming.mu.Lock()
		if s.endpointWithEvent.incoming.mu.asserted && s.endpoint.ep.Readiness(waiter.EventIn) == 0 {
			err = s.endpointWithEvent.local.SignalPeer(zxsocket.SignalIncoming, 0)
			s.endpointWithEvent.incoming.mu.asserted = false
		}
		s.incoming.mu.Unlock()
		if err != nil {
			panic(err)
		}
	}
	var addr *fidlnet.SocketAddress
	if wantAddr {
		sockaddr := toNetSocketAddress(s.netProto, sender)
		addr = &sockaddr
	}
	var truncated uint32
	if t := len(v) - int(dataLen); t > 0 {
		truncated = uint32(t)
		v = v[:dataLen]
	}
	return socket.DatagramSocketRecvMsgResultWithResponse(socket.DatagramSocketRecvMsgResponse{
		Addr:      addr,
		Data:      v,
		Truncated: truncated,
	}), nil
}

// NB: Due to another soft transition that happened, SendMsg is the "final
// state" we want to get at, while SendMsg2 is the "old" one.
func (s *datagramSocketImpl) SendMsg(_ fidl.Context, addr *fidlnet.SocketAddress, data []uint8, control socket.SendControlData, flags socket.SendMsgFlags) (socket.DatagramSocketSendMsgResult, error) {
	var writeOpts tcpip.WriteOptions
	if addr != nil {
		addr, err := toTCPIPFullAddress(*addr)
		if err != nil {
			return socket.DatagramSocketSendMsgResultWithErr(tcpipErrorToCode(tcpip.ErrBadAddress)), nil
		}
		if s.endpoint.netProto == ipv4.ProtocolNumber && len(addr.Addr) == header.IPv6AddressSize {
			return socket.DatagramSocketSendMsgResultWithErr(tcpipErrorToCode(tcpip.ErrAddressFamilyNotSupported)), nil
		}
		writeOpts.To = &addr
	}
	// TODO(https://fxbug.dev/21106): do something with control.
	for {
		n, resCh, err := s.ep.Write(tcpip.SlicePayload(data), writeOpts)
		if resCh != nil {
			if err != tcpip.ErrNoLinkAddress {
				panic(fmt.Sprintf("err=%v inconsistent with presence of resCh", err))
			}
			<-resCh
			continue
		}
		if err != nil {
			return socket.DatagramSocketSendMsgResultWithErr(tcpipErrorToCode(err)), nil
		}
		return socket.DatagramSocketSendMsgResultWithResponse(socket.DatagramSocketSendMsgResponse{Len: n}), nil
	}
}

type streamSocketImpl struct {
	*endpointWithSocket

	cancel context.CancelFunc
}

var _ socket.StreamSocketWithCtx = (*streamSocketImpl)(nil)

func newStreamSocket(eps *endpointWithSocket) (socket.StreamSocketWithCtxInterface, error) {
	localC, peerC, err := zx.NewChannel(0)
	if err != nil {
		return socket.StreamSocketWithCtxInterface{}, err
	}
	s := &streamSocketImpl{
		endpointWithSocket: eps,
	}
	s.addConnection(context.Background(), io.NodeWithCtxInterfaceRequest{Channel: localC})
	syslog.VLogTf(syslog.DebugVerbosity, "NewStream", "%p", s.endpointWithSocket)
	return socket.StreamSocketWithCtxInterface{Channel: peerC}, nil
}

func (s *streamSocketImpl) close() {
	if s.endpoint.decRef() {
		var linger tcpip.LingerOption
		if err := s.ep.GetSockOpt(&linger); err != nil {
			panic(fmt.Sprintf("GetSockOpt(%T): %s", linger, err))
		}

		doClose := func() {
			s.endpointWithSocket.close()

			if err := s.peer.Close(); err != nil {
				panic(err)
			}
		}

		if linger.Enabled {
			// `man 7 socket`:
			//
			//  When enabled, a close(2) or shutdown(2) will not return until all
			//  queued messages for the socket have been successfully sent or the
			//  linger timeout has been reached. Otherwise, the call returns
			//  immediately and the closing is done in the background. When the
			//  socket is closed as part of exit(2), it always lingers in the
			//  background.
			//
			// Thus we must allow linger-amount-of-time for pending writes to flush,
			// and do so synchronously if linger is enabled.
			time.AfterFunc(linger.Timeout, func() { close(s.linger) })
			doClose()
		} else {
			// Here be dragons.
			//
			// Normally, with linger disabled, the socket is immediately closed to
			// the application (accepting no more writes) and lingers for TCP_LINGER2
			// duration. However, because of the use of a zircon socket in front of
			// the netstack endpoint, we can't be sure that all writes have flushed
			// from the zircon socket to the netstack endpoint when we observe
			// `close(3)`. This in turn means we can't close the netstack endpoint
			// (which would start the TCP_LINGER2 timer), because there may still be
			// data pending in the zircon socket (e.g. when the netstack endpoint's
			// send buffer is full). We need *some* condition to break us out of this
			// deadlock.
			//
			// We pick TCP_LINGER2 somewhat arbitrarily. In the worst case, this
			// means that our true TCP linger time will be twice the configured
			// value, but this is the best we can do without rethinking the
			// interfaces.
			var linger tcpip.TCPLingerTimeoutOption
			if err := s.ep.GetSockOpt(&linger); err != nil {
				panic(fmt.Sprintf("GetSockOpt(%T): %s", linger, err))
			}
			time.AfterFunc(time.Duration(linger), func() { close(s.linger) })
			go doClose()
		}
	}

	s.cancel()
}

func (s *streamSocketImpl) Close(fidl.Context) (int32, error) {
	syslog.VLogTf(syslog.DebugVerbosity, "Close", "%p", s.endpointWithSocket)
	s.close()
	return int32(zx.ErrOk), nil
}

func (s *streamSocketImpl) addConnection(_ fidl.Context, object io.NodeWithCtxInterfaceRequest) {
	{
		sCopy := *s
		s := &sCopy

		s.ns.stats.SocketCount.Increment()
		s.endpoint.incRef()
		go func() {
			defer s.ns.stats.SocketCount.Decrement()

			ctx, cancel := context.WithCancel(context.Background())
			s.cancel = cancel
			defer func() {
				// Avoid double close when the peer calls Close and then hangs up.
				if ctx.Err() == nil {
					s.close()
				}
			}()

			stub := socket.StreamSocketWithCtxStub{Impl: s}
			component.ServeExclusive(ctx, &stub, object.Channel, func(err error) {
				// NB: this protocol is not discoverable, so the bindings do not include its name.
				_ = syslog.WarnTf("fuchsia.posix.socket.StreamSocket", "%s", err)
			})
		}()
	}
}

func (s *streamSocketImpl) Clone(ctx fidl.Context, flags uint32, object io.NodeWithCtxInterfaceRequest) error {
	s.addConnection(ctx, object)

	syslog.VLogTf(syslog.DebugVerbosity, "Clone", "%p: flags=%b", s.endpointWithSocket, flags)

	return nil
}

func (s *streamSocketImpl) Describe(fidl.Context) (io.NodeInfo, error) {
	var info io.NodeInfo
	h, err := s.endpointWithSocket.peer.Handle().Duplicate(zx.RightsBasic | zx.RightRead | zx.RightWrite)
	syslog.VLogTf(syslog.DebugVerbosity, "Describe", "%p: err=%v", s.endpointWithSocket, err)
	if err != nil {
		return info, err
	}
	info.SetStreamSocket(io.StreamSocket{Socket: zx.Socket(h)})
	return info, nil
}

func (s *streamSocketImpl) Accept(_ fidl.Context, wantAddr bool) (socket.StreamSocketAcceptResult, error) {
	code, addr, eps, err := s.endpointWithSocket.Accept(wantAddr)
	if err != nil {
		return socket.StreamSocketAcceptResult{}, err
	}
	if code != 0 {
		return socket.StreamSocketAcceptResultWithErr(code), nil
	}
	streamSocketInterface, err := newStreamSocket(eps)
	if err != nil {
		return socket.StreamSocketAcceptResult{}, err
	}
	response := socket.StreamSocketAcceptResponse{
		S: streamSocketInterface,
	}
	if addr != nil {
		sockaddr := toNetSocketAddress(s.netProto, *addr)
		response.Addr = &sockaddr
	}
	return socket.StreamSocketAcceptResultWithResponse(response), nil
}

func (ns *Netstack) onAddEndpoint(handle zx.Handle, ep tcpip.Endpoint) {
	if ep, loaded := ns.endpoints.LoadOrStore(handle, ep); loaded {
		var info stack.TransportEndpointInfo
		switch t := ep.Info().(type) {
		case *stack.TransportEndpointInfo:
			info = *t
		}
		syslog.Errorf("endpoint map store error, key %d exists with endpoint %+v", handle, info)
	}

	ns.stats.SocketsCreated.Increment()
}

func (ns *Netstack) onRemoveEndpoint(handle zx.Handle) {
	ns.endpoints.Delete(handle)
	ns.stats.SocketsDestroyed.Increment()
}

type providerImpl struct {
	ns *Netstack
}

var _ socket.ProviderWithCtx = (*providerImpl)(nil)

func toTransProtoStream(domain socket.Domain, proto socket.StreamSocketProtocol) (posix.Errno, tcpip.TransportProtocolNumber) {
	switch proto {
	case socket.StreamSocketProtocolTcp:
		return 0, tcp.ProtocolNumber
	}
	return posix.ErrnoEprotonosupport, 0
}

func toTransProtoDatagram(domain socket.Domain, proto socket.DatagramSocketProtocol) (posix.Errno, tcpip.TransportProtocolNumber) {
	switch proto {
	case socket.DatagramSocketProtocolUdp:
		return 0, udp.ProtocolNumber
	case socket.DatagramSocketProtocolIcmpEcho:
		switch domain {
		case socket.DomainIpv4:
			return 0, icmp.ProtocolNumber4
		case socket.DomainIpv6:
			return 0, icmp.ProtocolNumber6
		}
	}
	return posix.ErrnoEprotonosupport, 0
}

func toNetProto(domain socket.Domain) (posix.Errno, tcpip.NetworkProtocolNumber) {
	switch domain {
	case socket.DomainIpv4:
		return 0, ipv4.ProtocolNumber
	case socket.DomainIpv6:
		return 0, ipv6.ProtocolNumber
	default:
		return posix.ErrnoEpfnosupport, 0
	}
}

type callback func(*waiter.Entry)

func (cb callback) Callback(e *waiter.Entry) {
	cb(e)
}

func (sp *providerImpl) DatagramSocket(ctx fidl.Context, domain socket.Domain, proto socket.DatagramSocketProtocol) (socket.ProviderDatagramSocketResult, error) {
	code, netProto := toNetProto(domain)
	if code != 0 {
		return socket.ProviderDatagramSocketResultWithErr(code), nil
	}
	code, transProto := toTransProtoDatagram(domain, proto)
	if code != 0 {
		return socket.ProviderDatagramSocketResultWithErr(code), nil
	}

	wq := new(waiter.Queue)
	ep, tcpErr := sp.ns.stack.NewEndpoint(transProto, netProto, wq)
	if tcpErr != nil {
		return socket.ProviderDatagramSocketResultWithErr(tcpipErrorToCode(tcpErr)), nil
	}

	var localE, peerE zx.Handle
	if status := zx.Sys_eventpair_create(0, &localE, &peerE); status != zx.ErrOk {
		return socket.ProviderDatagramSocketResult{}, &zx.Error{Status: status, Text: "zx.EventPair"}
	}
	localC, peerC, err := zx.NewChannel(0)
	if err != nil {
		return socket.ProviderDatagramSocketResult{}, err
	}
	s := &datagramSocketImpl{
		endpointWithEvent: &endpointWithEvent{
			endpoint: endpoint{
				ep:         ep,
				wq:         wq,
				transProto: transProto,
				netProto:   netProto,
				ns:         sp.ns,
			},
			local: localE,
			peer:  peerE,
		},
	}

	s.entry.Callback = callback(func(*waiter.Entry) {
		var err error
		s.endpointWithEvent.incoming.mu.Lock()
		if !s.endpointWithEvent.incoming.mu.asserted && s.endpoint.ep.Readiness(waiter.EventIn) != 0 {
			err = s.endpointWithEvent.local.SignalPeer(0, zxsocket.SignalIncoming)
			s.endpointWithEvent.incoming.mu.asserted = true
		}
		s.endpointWithEvent.incoming.mu.Unlock()
		if err != nil {
			panic(err)
		}
	})

	s.wq.EventRegister(&s.entry, waiter.EventIn)

	s.addConnection(ctx, io.NodeWithCtxInterfaceRequest{Channel: localC})
	syslog.VLogTf(syslog.DebugVerbosity, "NewDatagram", "%p", s.endpointWithEvent)
	datagramSocketInterface := socket.DatagramSocketWithCtxInterface{Channel: peerC}

	sp.ns.onAddEndpoint(localE, ep)

	if err := s.endpointWithEvent.local.SignalPeer(0, zxsocket.SignalOutgoing); err != nil {
		panic(fmt.Sprintf("local.SignalPeer(0, zxsocket.SignalOutgoing) = %s", err))
	}

	return socket.ProviderDatagramSocketResultWithResponse(socket.ProviderDatagramSocketResponse{
		S: socket.DatagramSocketWithCtxInterface{Channel: datagramSocketInterface.Channel},
	}), nil

}

func (sp *providerImpl) StreamSocket(ctx fidl.Context, domain socket.Domain, proto socket.StreamSocketProtocol) (socket.ProviderStreamSocketResult, error) {
	code, netProto := toNetProto(domain)
	if code != 0 {
		return socket.ProviderStreamSocketResultWithErr(code), nil
	}
	code, transProto := toTransProtoStream(domain, proto)
	if code != 0 {
		return socket.ProviderStreamSocketResultWithErr(code), nil
	}

	wq := new(waiter.Queue)
	ep, tcpErr := sp.ns.stack.NewEndpoint(transProto, netProto, wq)
	if tcpErr != nil {
		return socket.ProviderStreamSocketResultWithErr(tcpipErrorToCode(tcpErr)), nil
	}

	socketEp, err := newEndpointWithSocket(ep, wq, transProto, netProto, sp.ns)
	if err != nil {
		return socket.ProviderStreamSocketResult{}, err
	}
	streamSocketInterface, err := newStreamSocket(socketEp)
	if err != nil {
		return socket.ProviderStreamSocketResult{}, err
	}
	return socket.ProviderStreamSocketResultWithResponse(socket.ProviderStreamSocketResponse{
		S: socket.StreamSocketWithCtxInterface{Channel: streamSocketInterface.Channel},
	}), nil
}

func (sp *providerImpl) InterfaceIndexToName(_ fidl.Context, index uint64) (socket.ProviderInterfaceIndexToNameResult, error) {
	if info, ok := sp.ns.stack.NICInfo()[tcpip.NICID(index)]; ok {
		return socket.ProviderInterfaceIndexToNameResultWithResponse(socket.ProviderInterfaceIndexToNameResponse{
			Name: info.Name,
		}), nil
	}
	return socket.ProviderInterfaceIndexToNameResultWithErr(int32(zx.ErrNotFound)), nil
}

func (sp *providerImpl) InterfaceNameToIndex(_ fidl.Context, name string) (socket.ProviderInterfaceNameToIndexResult, error) {
	for id, info := range sp.ns.stack.NICInfo() {
		if info.Name == name {
			return socket.ProviderInterfaceNameToIndexResultWithResponse(socket.ProviderInterfaceNameToIndexResponse{
				Index: uint64(id),
			}), nil
		}
	}
	return socket.ProviderInterfaceNameToIndexResultWithErr(int32(zx.ErrNotFound)), nil
}

// Adapted from helper function `nicStateFlagsToLinux` in gvisor's
// sentry/socket/netstack package.
func nicInfoFlagsToFIDL(info stack.NICInfo) socket.InterfaceFlags {
	ifs := info.Context.(*ifState)
	var bits socket.InterfaceFlags
	flags := info.Flags
	if flags.Loopback {
		bits |= socket.InterfaceFlagsLoopback
	}
	if flags.Running {
		bits |= socket.InterfaceFlagsRunning
	}
	if flags.Promiscuous {
		bits |= socket.InterfaceFlagsPromisc
	}
	// Check `IsUpLocked` because netstack interfaces are always defined to be
	// `Up` in gVisor.
	ifs.mu.Lock()
	if ifs.IsUpLocked() {
		bits |= socket.InterfaceFlagsUp
	}
	ifs.mu.Unlock()
	// Approximate that all interfaces support multicasting.
	bits |= socket.InterfaceFlagsMulticast
	return bits
}

func (sp *providerImpl) InterfaceNameToFlags(_ fidl.Context, name string) (socket.ProviderInterfaceNameToFlagsResult, error) {
	for _, info := range sp.ns.stack.NICInfo() {
		if info.Name == name {
			return socket.ProviderInterfaceNameToFlagsResultWithResponse(socket.ProviderInterfaceNameToFlagsResponse{
				Flags: nicInfoFlagsToFIDL(info),
			}), nil
		}
	}
	return socket.ProviderInterfaceNameToFlagsResultWithErr(int32(zx.ErrNotFound)), nil
}

func (sp *providerImpl) GetInterfaceAddresses(fidl.Context) ([]socket.InterfaceAddresses, error) {
	nicInfos := sp.ns.stack.NICInfo()

	resultInfos := make([]socket.InterfaceAddresses, 0, len(nicInfos))
	for id, info := range nicInfos {
		// Ensure deterministic API response.
		sort.Slice(info.ProtocolAddresses, func(i, j int) bool {
			x, y := info.ProtocolAddresses[i], info.ProtocolAddresses[j]
			if x.Protocol != y.Protocol {
				return x.Protocol < y.Protocol
			}
			ax, ay := x.AddressWithPrefix, y.AddressWithPrefix
			if ax.Address != ay.Address {
				return ax.Address < ay.Address
			}
			return ax.PrefixLen < ay.PrefixLen
		})

		addrs := make([]fidlnet.Subnet, 0, len(info.ProtocolAddresses))
		for _, a := range info.ProtocolAddresses {
			if a.Protocol != ipv4.ProtocolNumber && a.Protocol != ipv6.ProtocolNumber {
				continue
			}
			addrs = append(addrs, fidlnet.Subnet{
				Addr:      fidlconv.ToNetIpAddress(a.AddressWithPrefix.Address),
				PrefixLen: uint8(a.AddressWithPrefix.PrefixLen),
			})
		}

		var resultInfo socket.InterfaceAddresses
		resultInfo.SetId(uint64(id))
		resultInfo.SetName(info.Name)
		resultInfo.SetAddresses(addrs)

		// gVisor assumes interfaces are always up, which is not the case on Fuchsia,
		// so overwrite it with Fuchsia's interface state.
		bits := nicInfoFlagsToFIDL(info)
		// TODO(fxbug.dev/64758): don't `SetFlags` once all clients are
		// transitioned to use `interface_flags`.
		resultInfo.SetFlags(uint32(bits))
		resultInfo.SetInterfaceFlags(bits)

		resultInfos = append(resultInfos, resultInfo)
	}

	// Ensure deterministic API response.
	sort.Slice(resultInfos, func(i, j int) bool {
		return resultInfos[i].Id < resultInfos[j].Id
	})
	return resultInfos, nil
}

func tcpipErrorToCode(err *tcpip.Error) posix.Errno {
	if err != tcpip.ErrConnectStarted {
		if pc, file, line, ok := runtime.Caller(1); ok {
			if i := strings.LastIndexByte(file, '/'); i != -1 {
				file = file[i+1:]
			}
			syslog.VLogf(syslog.DebugVerbosity, "%s: %s:%d: %s", runtime.FuncForPC(pc).Name(), file, line, err)
		} else {
			syslog.VLogf(syslog.DebugVerbosity, "%s", err)
		}
	}
	switch err {
	case tcpip.ErrUnknownProtocol:
		return posix.ErrnoEinval
	case tcpip.ErrUnknownNICID:
		return posix.ErrnoEinval
	case tcpip.ErrUnknownDevice:
		return posix.ErrnoEnodev
	case tcpip.ErrUnknownProtocolOption:
		return posix.ErrnoEnoprotoopt
	case tcpip.ErrDuplicateNICID:
		return posix.ErrnoEexist
	case tcpip.ErrDuplicateAddress:
		return posix.ErrnoEexist
	case tcpip.ErrNoRoute:
		return posix.ErrnoEhostunreach
	case tcpip.ErrBadLinkEndpoint:
		return posix.ErrnoEinval
	case tcpip.ErrAlreadyBound:
		return posix.ErrnoEinval
	case tcpip.ErrInvalidEndpointState:
		return posix.ErrnoEinval
	case tcpip.ErrAlreadyConnecting:
		return posix.ErrnoEalready
	case tcpip.ErrAlreadyConnected:
		return posix.ErrnoEisconn
	case tcpip.ErrNoPortAvailable:
		return posix.ErrnoEagain
	case tcpip.ErrPortInUse:
		return posix.ErrnoEaddrinuse
	case tcpip.ErrBadLocalAddress:
		return posix.ErrnoEaddrnotavail
	case tcpip.ErrClosedForSend:
		return posix.ErrnoEpipe
	case tcpip.ErrClosedForReceive:
		return posix.ErrnoEagain
	case tcpip.ErrWouldBlock:
		return posix.Ewouldblock
	case tcpip.ErrConnectionRefused:
		return posix.ErrnoEconnrefused
	case tcpip.ErrTimeout:
		return posix.ErrnoEtimedout
	case tcpip.ErrAborted:
		return posix.ErrnoEpipe
	case tcpip.ErrConnectStarted:
		return posix.ErrnoEinprogress
	case tcpip.ErrDestinationRequired:
		return posix.ErrnoEdestaddrreq
	case tcpip.ErrNotSupported:
		return posix.ErrnoEopnotsupp
	case tcpip.ErrQueueSizeNotSupported:
		return posix.ErrnoEnotty
	case tcpip.ErrNotConnected:
		return posix.ErrnoEnotconn
	case tcpip.ErrConnectionReset:
		return posix.ErrnoEconnreset
	case tcpip.ErrConnectionAborted:
		return posix.ErrnoEconnaborted
	case tcpip.ErrNoSuchFile:
		return posix.ErrnoEnoent
	case tcpip.ErrInvalidOptionValue:
		return posix.ErrnoEinval
	case tcpip.ErrNoLinkAddress:
		return posix.ErrnoEhostdown
	case tcpip.ErrBadAddress:
		return posix.ErrnoEfault
	case tcpip.ErrNetworkUnreachable:
		return posix.ErrnoEnetunreach
	case tcpip.ErrMessageTooLong:
		return posix.ErrnoEmsgsize
	case tcpip.ErrNoBufferSpace:
		return posix.ErrnoEnobufs
	case tcpip.ErrBroadcastDisabled, tcpip.ErrNotPermitted:
		return posix.ErrnoEacces
	case tcpip.ErrAddressFamilyNotSupported:
		return posix.ErrnoEafnosupport
	default:
		panic(fmt.Sprintf("unknown error %v", err))
	}
}
