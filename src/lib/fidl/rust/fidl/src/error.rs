// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! Error (common to all fidl operations)

use {
    crate::handle::{ObjectType, Rights},
    fuchsia_zircon_status as zx_status,
    std::{io, result},
    thiserror::Error,
};

/// A specialized `Result` type for FIDL operations.
pub type Result<T> = result::Result<T, Error>;

/// The error type used by FIDL operations.
#[derive(Debug, Error)]
#[non_exhaustive]
pub enum Error {
    /// Unexpected response to synchronous FIDL query.
    ///
    /// This will occur if an event or a response with an unexpected transaction
    /// ID is received when using the synchronous FIDL bindings.
    #[error("Unexpected response to synchronous FIDL query.")]
    UnexpectedSyncResponse,

    /// Invalid header for a FIDL buffer.
    #[error("Invalid header for a FIDL buffer.")]
    InvalidHeader,

    /// Unsupported wire format.
    #[error("Incompatible wire format magic number: {}.", _0)]
    IncompatibleMagicNumber(u8),

    /// Invalid FIDL buffer.
    #[error("Invalid FIDL buffer.")]
    Invalid,

    /// The FIDL object could not fit within the provided buffer range.
    #[error("The FIDL object could not fit within the provided buffer range")]
    OutOfRange,

    /// Decoding the FIDL object did not use all of the bytes provided.
    #[error("Decoding the FIDL object did not use all of the bytes provided.")]
    ExtraBytes,

    /// Decoding the FIDL object did not use all of the handles provided.
    #[error("Decoding the FIDL object did not use all of the handles provided.")]
    ExtraHandles,

    /// Decoding the FIDL object observed non-zero value in a padding byte.
    #[error(
        "Decoding the FIDL object observed non-zero value in the padding at byte {}. \
                   Padding starts at byte {}.",
        non_zero_pos,
        padding_start
    )]
    NonZeroPadding {
        /// Index of the first byte of the padding, relative to the beginning of the message.
        padding_start: usize,
        /// Index of the byte in the padding that was non-zero, relative to the beginning of the
        /// message.
        non_zero_pos: usize,
    },

    /// The FIDL object had too many layers of out-of-line recursion.
    #[error("The FIDL object had too many layers of out-of-line recursion.")]
    MaxRecursionDepth,

    /// There was an attempt read or write a null-valued object as a non-nullable type.
    #[error(
        "There was an attempt to read or write a null-valued object as a non-nullable FIDL type."
    )]
    NotNullable,

    /// A FIDL object reference with nonzero byte length had a null data pointer.
    #[error("A FIDL object reference with nonzero byte length had a null data pointer.")]
    UnexpectedNullRef,

    /// Incorrectly encoded UTF8.
    #[error("A FIDL message contained incorrectly encoded UTF8.")]
    Utf8Error,

    /// A message was recieved for an ordinal value that the service does not understand.
    /// This generally results from an attempt to call a FIDL service of a type other than
    /// the one being served.
    #[error(
        "A message was received for ordinal value {} \
                   that the FIDL service {} does not understand.",
        ordinal,
        service_name
    )]
    UnknownOrdinal {
        /// The unknown ordinal.
        ordinal: u64,
        /// The name of the service for which the message was intended.
        service_name: &'static str,
    },

    /// Invalid bits value for a strict bits type.
    #[error("Invalid bits value for a strict bits type.")]
    InvalidBitsValue,

    /// Invalid enum value for a strict enum type.
    #[error("Invalid enum value for a strict enum type.")]
    InvalidEnumValue,

    /// Unrecognized descriminant for a FIDL union type.
    #[error("Unrecognized descriminant for a FIDL union type.")]
    UnknownUnionTag,

    /// A future was polled after it had already completed.
    #[error("A FIDL future was polled after it had already completed.")]
    PollAfterCompletion,

    /// A response message was received with txid 0.
    #[error("Invalid response with txid 0.")]
    InvalidResponseTxid,

    /// An envelope in the FIDL message had an unexpected number of bytes.
    #[error("Invalid number of bytes in FIDL envelope.")]
    InvalidNumBytesInEnvelope,

    /// An envelope in the FIDL message had an unexpected number of handles.
    #[error("Invalid number of handles in FIDL envelope.")]
    InvalidNumHandlesInEnvelope,

    /// A handle which is invalid in the context of a host build of Fuchsia.
    #[error("Invalid FIDL handle used on the host.")]
    InvalidHostHandle,

    /// The handle subtype doesn't match the expected subtype.
    #[error("Incorrect handle subtype. Expected {}, but received {}", expected.into_raw(), received.into_raw())]
    IncorrectHandleSubtype {
        /// The expected object type.
        expected: ObjectType,
        /// The received object type.
        received: ObjectType,
    },

    /// Some expected handle rights are missing.
    #[error("Some expected handle rights are missing: {}", missing_rights.bits())]
    MissingExpectedHandleRights {
        /// The rights that are missing.
        missing_rights: Rights,
    },

    /// A non-resource type encountered a handle when decoding unknown data
    #[error("Attempted to decode unknown data with handles for a non-resource type")]
    CannotStoreUnknownHandles,

    /// An error was encountered during handle replace().
    #[error("An error was encountered during handle replace()")]
    HandleReplace(#[source] zx_status::Status),

    /// A FIDL server encountered an IO error writing a response to a channel.
    #[error("A server encountered an IO error writing a FIDL response to a channel: {}", _0)]
    ServerResponseWrite(#[source] zx_status::Status),

    /// A FIDL server encountered an IO error reading incoming requests from a channel.
    #[error(
        "A FIDL server encountered an IO error reading incoming FIDL requests from a channel: {}",
        _0
    )]
    ServerRequestRead(#[source] zx_status::Status),

    /// A FIDL server encountered an IO error writing an epitaph to a channel.
    #[error("A FIDL server encountered an IO error writing an epitaph into a channel: {}", _0)]
    ServerEpitaphWrite(#[source] zx_status::Status),

    /// A FIDL client encountered an IO error reading a response from a channel.
    #[error("A FIDL client encountered an IO error reading a response from a channel: {}", _0)]
    ClientRead(#[source] zx_status::Status),

    /// A FIDL client encountered an IO error writing a request to a channel.
    #[error("A FIDL client encountered an IO error writing a request into a channel: {}", _0)]
    ClientWrite(#[source] zx_status::Status),

    /// A FIDL client's channel was closed. Contains an epitaph if one was sent by the server, or
    /// `zx_status::Status::PEER_CLOSED` otherwise.
    #[error("A FIDL client's channel to the service {} was closed: {}", service_name, status)]
    ClientChannelClosed {
        /// The epitaph or `zx_status::Status::PEER_CLOSED`.
        #[source]
        status: zx_status::Status,
        /// The name of the service at the other end of the channel.
        service_name: &'static str,
    },

    /// There was an error creating a channel to be used for a FIDL client-server pair.
    #[error(
        "There was an error creating a channel to be used for a FIDL client-server pair: {}",
        _0
    )]
    ChannelPairCreate(#[source] zx_status::Status),

    /// There was an error attaching a FIDL channel to the Tokio reactor.
    #[error("There was an error attaching a FIDL channel to the Tokio reactor: {}", _0)]
    AsyncChannel(#[source] io::Error),

    /// There was a miscellaneous io::Error during a test.
    #[cfg(target_os = "fuchsia")]
    #[cfg(test)]
    #[error("Test zx_status::Status: {}", _0)]
    TestIo(#[source] zx_status::Status),
}

impl Error {
    /// Returns `true` if the error was sourced by a closed channel.
    pub fn is_closed(&self) -> bool {
        self.is_closed_impl()
    }

    #[cfg(target_os = "fuchsia")]
    fn is_closed_impl(&self) -> bool {
        match self {
            Error::ClientRead(zx_status::Status::PEER_CLOSED)
            | Error::ClientWrite(zx_status::Status::PEER_CLOSED)
            | Error::ClientChannelClosed { .. }
            | Error::ServerRequestRead(zx_status::Status::PEER_CLOSED)
            | Error::ServerResponseWrite(zx_status::Status::PEER_CLOSED)
            | Error::ServerEpitaphWrite(zx_status::Status::PEER_CLOSED) => true,
            _ => false,
        }
    }

    #[cfg(not(target_os = "fuchsia"))]
    fn is_closed_impl(&self) -> bool {
        false
    }
}
