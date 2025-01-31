// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! Controls one link to another node over a zx socket.

use {
    anyhow::Error,
    byteorder::WriteBytesExt,
    futures::{io::AsyncWriteExt, prelude::*},
    overnet_core::{ConfigProducer, Router},
    std::{sync::Arc, time::Duration},
    stream_framer::{new_deframer, new_framer, Deframed, Format, ReadBytes},
};

pub fn run_stream_link<'a>(
    node: Arc<Router>,
    rx_bytes: &'a mut (dyn AsyncRead + Unpin + Send),
    tx_bytes: &'a mut (dyn AsyncWrite + Unpin + Send),
    config: ConfigProducer,
) -> impl 'a + Send + Future<Output = Result<(), Error>> {
    let (mut link_sender, mut link_receiver) = node.new_link(config);
    drop(node);

    let (mut framer, mut framer_read) = new_framer(LosslessBinary, 4096);
    let (mut deframer_write, mut deframer) = new_deframer(LosslessBinary);

    futures::future::try_join4(
        async move {
            loop {
                let msg = framer_read.read().await?;
                tx_bytes.write(&msg).await?;
            }
        },
        async move {
            let mut buf = [0u8; 4096];
            loop {
                let n = rx_bytes.read(&mut buf).await?;
                if n == 0 {
                    return Ok::<_, Error>(());
                }
                deframer_write.write(&buf[..n]).await?;
            }
        },
        async move {
            loop {
                if let ReadBytes::Framed(mut frame) = deframer.read().await? {
                    link_receiver.received_frame(frame.as_mut()).await;
                }
            }
        },
        async move {
            while let Some(frame) = link_sender.next_send().await {
                framer.write(frame.bytes()).await?;
            }
            Ok::<_, Error>(())
        },
    )
    .map_ok(|((), (), (), ())| ())
}

/// Framing format that assumes a lossless underlying byte stream that can transport all 8 bits of a
/// byte.
struct LosslessBinary;

impl Format for LosslessBinary {
    fn frame(&self, bytes: &[u8], outgoing: &mut Vec<u8>) -> Result<(), Error> {
        if bytes.len() > (std::u16::MAX as usize) + 1 {
            return Err(anyhow::format_err!(
                "Packet length ({}) too long for stream framing",
                bytes.len()
            ));
        }
        outgoing.write_u16::<byteorder::LittleEndian>((bytes.len() - 1) as u16)?;
        outgoing.extend_from_slice(bytes);
        Ok(())
    }

    fn deframe(&self, bytes: &[u8]) -> Result<Deframed, Error> {
        if bytes.len() <= 3 {
            return Ok(Deframed { frame: None, unframed_bytes: 0, new_start_pos: 0 });
        }
        let len = 1 + (u16::from_le_bytes([bytes[0], bytes[1]]) as usize);
        if bytes.len() < 2 + len {
            // Not enough bytes to deframe: done for now.
            return Ok(Deframed { frame: None, unframed_bytes: 0, new_start_pos: 0 });
        }
        let frame = &bytes[2..2 + len];
        return Ok(Deframed {
            frame: Some(frame.to_vec()),
            unframed_bytes: 0,
            new_start_pos: 2 + len,
        });
    }

    fn deframe_timeout(&self, _have_pending_bytes: bool) -> Option<Duration> {
        None
    }
}

#[cfg(test)]
mod test {
    use super::*;

    #[fuchsia_async::run(1, test)]
    async fn simple_frame() -> Result<(), Error> {
        let (mut framer_writer, mut framer_reader) = new_framer(LosslessBinary, 1024);
        framer_writer.write(&[1, 2, 3, 4]).await?;
        let (mut deframer_writer, mut deframer_reader) = new_deframer(LosslessBinary);
        deframer_writer.write(framer_reader.read().await?.as_slice()).await?;
        assert_eq!(deframer_reader.read().await?, ReadBytes::Framed(vec![1, 2, 3, 4]));
        framer_writer.write(&[5, 6, 7, 8]).await?;
        deframer_writer.write(framer_reader.read().await?.as_slice()).await?;
        assert_eq!(deframer_reader.read().await?, ReadBytes::Framed(vec![5, 6, 7, 8]));
        Ok(())
    }

    #[fuchsia_async::run(1, test)]
    async fn large_frame() -> Result<(), Error> {
        let big_slice = vec![0u8; 100000];
        let (mut framer_writer, _framer_reader) = new_framer(LosslessBinary, 1024);
        assert!(framer_writer.write(&big_slice).await.is_err());
        Ok(())
    }
}
