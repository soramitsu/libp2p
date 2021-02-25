/**
 * Copyright Soramitsu Co., Ltd. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#include <libp2p/muxer/yamux/yamux_stream.hpp>

#include <cassert>

#include <libp2p/muxer/yamux/yamux_error.hpp>

#define TRACE_ENABLED 0
#include <libp2p/common/trace.hpp>

namespace libp2p::connection {

  namespace {
    auto log() {
      static auto logger = common::createLogger("yx-stream");
      return logger.get();
    }
  }  // namespace

  YamuxStream::YamuxStream(
      std::shared_ptr<connection::SecureConnection> connection,
      YamuxStreamFeedback &feedback, uint32_t stream_id, size_t window_size,
      size_t maximum_window_size, size_t write_queue_limit)
      : connection_(std::move(connection)),
        feedback_(feedback),
        stream_id_(stream_id),
        send_window_size_(window_size),
        receive_window_size_(window_size),
        maximum_window_size_(maximum_window_size),
        write_queue_(write_queue_limit) {
    assert(connection_);
    assert(stream_id_ > 0);
    assert(send_window_size_ <= maximum_window_size_);
    assert(receive_window_size_ <= maximum_window_size_);
    assert(write_queue_limit >= maximum_window_size_);
  }

  void YamuxStream::read(gsl::span<uint8_t> out, size_t bytes,
                         ReadCallbackFunc cb) {
    doRead(out, bytes, std::move(cb), false);
  }

  void YamuxStream::readSome(gsl::span<uint8_t> out, size_t bytes,
                             ReadCallbackFunc cb) {
    doRead(out, bytes, std::move(cb), true);
  }

  void YamuxStream::deferReadCallback(outcome::result<size_t> res,
                                      ReadCallbackFunc cb) {
    if (no_more_callbacks_) {
      log()->debug("{} closed by client, ignoring callback");
      return;
    }
    feedback_.deferCall([wptr = weak_from_this(), res, cb = std::move(cb)]() {
      auto self = wptr.lock();
      if (self && !self->no_more_callbacks_) {
        cb(res);
      }
    });
  }

  void YamuxStream::write(gsl::span<const uint8_t> in, size_t bytes,
                          WriteCallbackFunc cb) {
    doWrite(in, bytes, std::move(cb), false);
  }

  void YamuxStream::writeSome(gsl::span<const uint8_t> in, size_t bytes,
                              WriteCallbackFunc cb) {
    doWrite(in, bytes, std::move(cb), true);
  }

  void YamuxStream::deferWriteCallback(std::error_code ec,
                                       WriteCallbackFunc cb) {
    if (no_more_callbacks_) {
      log()->debug("{} closed by client, ignoring callback");
      return;
    }
    feedback_.deferCall([wptr = weak_from_this(), ec, cb = std::move(cb)]() {
      auto self = wptr.lock();
      if (self && !self->no_more_callbacks_) {
        cb(ec);
      }
    });
  }

  bool YamuxStream::isClosed() const noexcept {
    return close_reason_.value() != 0;
  }

  void YamuxStream::close(VoidResultHandlerFunc cb) {
    close_cb_ = std::move(cb);

    if (isClosed()) {
      if (close_cb_) {
        feedback_.deferCall([wptr = weak_from_this()] {
          auto self = wptr.lock();
          if (self) {
            self->closeCompleted();
          }
        });
      }
      return;
    }

    if (!isClosedForWrite()) {
      // closing for writes
      is_writable_ = false;

      // sends FIN after data is sent
      doWrite();
    }
  }

  void YamuxStream::closeCompleted() {
    if (!close_reason_) {
      close_reason_ = YamuxError::STREAM_CLOSED_BY_HOST;
    }
    if (close_cb_) {
      auto cb = std::move(close_cb_);
      close_cb_ = decltype(close_cb_){};
      if (close_reason_ == YamuxError::STREAM_CLOSED_BY_HOST) {
        cb(outcome::success());
      } else {
        cb(close_reason_);
      }
    }
  }

  bool YamuxStream::isClosedForRead() const noexcept {
    return !is_readable_;
  }

  bool YamuxStream::isClosedForWrite() const noexcept {
    return !is_writable_;
  }

  void YamuxStream::reset() {
    is_readable_ = false;
    is_writable_ = false;
    no_more_callbacks_ = true;
    close_reason_ = YamuxError::STREAM_RESET_BY_HOST;
    write_queue_.clear();
    internal_read_buffer_.clear();
    read_cb_ = decltype(read_cb_){};
    window_size_cb_ = decltype(window_size_cb_){};
    close_cb_ = decltype(close_cb_){};
    feedback_.resetStream(stream_id_);
  }

  void YamuxStream::adjustWindowSize(uint32_t new_size,
                                     VoidResultHandlerFunc cb) {
    if (close_reason_ || new_size > maximum_window_size_
        || new_size < receive_window_size_) {
      if (cb) {
        feedback_.deferCall([wptr = weak_from_this(), cb = std::move(cb)]() {
          auto self = wptr.lock();
          if (!self) {
            return;
          }
          if (!self->close_reason_) {
            cb(YamuxError::INVALID_WINDOW_SIZE);
          } else {
            cb(self->close_reason_);
          }
        });
      }
      return;
    }

    feedback_.ackReceivedBytes(stream_id_, new_size - receive_window_size_);

    if (cb) {
      window_size_cb_ = [wptr = weak_from_this(), cb = std::move(cb),
                         new_size](auto &&res) {
        auto self = wptr.lock();
        if (!self) {
          return;
        }
        if (self->close_reason_) {
          cb(self->close_reason_);
        } else if (self->receive_window_size_ >= new_size) {
          cb(outcome::success());
        } else {
          // continue waiting
          return;
        }
        self->window_size_cb_ = VoidResultHandlerFunc{};
      };
    }
  }

  outcome::result<peer::PeerId> YamuxStream::remotePeerId() const {
    return connection_->remotePeer();
  }

  outcome::result<bool> YamuxStream::isInitiator() const {
    return connection_->isInitiator();
  }

  outcome::result<multi::Multiaddress> YamuxStream::localMultiaddr() const {
    return connection_->localMultiaddr();
  }

  outcome::result<multi::Multiaddress> YamuxStream::remoteMultiaddr() const {
    return connection_->remoteMultiaddr();
  }

  void YamuxStream::increaseSendWindow(size_t delta) {
    send_window_size_ += delta;
    TRACE("stream {} send window changed by {} to {}", stream_id_,
                 delta, send_window_size_);
    doWrite();
  }

  YamuxStream::DataFromConnectionResult YamuxStream::onDataRead(
      gsl::span<uint8_t> bytes, bool fin, bool rst) {
    auto sz = static_cast<size_t>(bytes.size());
    TRACE("stream {} read {} bytes", stream_id_, sz);

    bool overflow = false;
    bool read_completed = false;
    size_t bytes_consumed = 0;

    // First transfer bytes to client if available
    if (sz > 0) {
      if (is_reading_) {
        auto bytes_needed = static_cast<size_t>(external_read_buffer_.size());

        assert(bytes_needed > 0);
        assert(internal_read_buffer_.empty());

        // if sz > bytes_needed then internal buffer will be non empty after
        // this
        bytes_consumed =
            internal_read_buffer_.addAndConsume(bytes, external_read_buffer_);

        assert(bytes_consumed > 0);

        external_read_buffer_ = external_read_buffer_.subspan(bytes_consumed);

        read_completed = external_read_buffer_.empty();
        if (reading_some_) {
          read_message_size_ = bytes_consumed;
          read_completed = true;
        }

        if (read_completed) {
          readCompleted();
          // after this call, state may change
        } else {
          assert(bytes_consumed < bytes_needed);
        }
      } else {
        internal_read_buffer_.add(bytes);
      }

      overflow = receive_window_size_
          < (internal_read_buffer_.size() + external_read_buffer_.size());
    }

    if (isClosed()) {
      // already closed, maybe error
      return kRemoveStreamAndSendRst;
    }

    if (rst) {
      doClose(YamuxError::STREAM_RESET_BY_PEER, false);
      return kRemoveStream;
    }

    if (fin) {
      is_readable_ = false;
      if (!is_writable_) {
        doClose(YamuxError::STREAM_CLOSED_BY_HOST, false);

        // connection will remove stream
        return kRemoveStream;
      }
      return kKeepStream;
    }

    if (overflow) {
      doClose(YamuxError::RECEIVE_WINDOW_OVERFLOW, false);
      return kRemoveStreamAndSendRst;
    }

    if (bytes_consumed > 0) {
      feedback_.ackReceivedBytes(stream_id_, bytes_consumed);
      receive_window_size_ += bytes_consumed;
    }

    return kKeepStream;
  }

  void YamuxStream::onDataWritten(size_t bytes) {
    if (!write_queue_.ack(bytes)) {
      log()->error("write queue ack failed, stream {}", stream_id_);
      feedback_.resetStream(stream_id_);
      doClose(YamuxError::INTERNAL_ERROR, true);
    }
  }

  void YamuxStream::closedByConnection(std::error_code ec) {
    doClose(ec, true);
  }

  void YamuxStream::doClose(std::error_code ec, bool notify_read_callback) {
    assert(ec);

    close_reason_ = ec;
    is_readable_ = false;
    is_writable_ = false;

    if (notify_read_callback) {
      internal_read_buffer_.clear();
      if (is_reading_) {
        is_reading_ = false;
        assert(read_cb_);
        auto cb = std::move(read_cb_);
        read_cb_ = ReadCallbackFunc{};
        if (!no_more_callbacks_) {
          cb(ec);
        }
      }
    }

    if (close_cb_) {
      closeCompleted();
    }

    if (!no_more_callbacks_) {
      write_queue_.broadcast(
          [this](const basic::Writer::WriteCallbackFunc &cb) -> bool {
            if (!no_more_callbacks_) {
              cb(close_reason_);
            }

            // reentrancy may occur here
            return !no_more_callbacks_;
          });

      write_queue_.clear();
    }
  }

  void YamuxStream::doRead(gsl::span<uint8_t> out, size_t bytes,
                           ReadCallbackFunc cb, bool some) {
    assert(cb);

    if (!cb || bytes == 0 || out.empty()
        || static_cast<size_t>(out.size()) < bytes) {
      return deferReadCallback(YamuxError::INVALID_ARGUMENT, std::move(cb));
    }

    // If something is still in read buffer, the client can consume these bytes
    auto bytes_available_now = internal_read_buffer_.size();
    if (bytes_available_now >= bytes || (some && bytes_available_now > 0)) {
      out = out.first(bytes);
      size_t consumed = internal_read_buffer_.consume(out);

      assert(consumed > 0);

      if (is_readable_) {
        feedback_.ackReceivedBytes(stream_id_, consumed);
      }
      return deferReadCallback(consumed, std::move(cb));
    }

    if (close_reason_) {
      return deferReadCallback(close_reason_, std::move(cb));
    }

    if (is_reading_) {
      return deferReadCallback(YamuxError::STREAM_IS_READING, std::move(cb));
    }

    if (!is_readable_) {
      // half closed
      return deferReadCallback(YamuxError::STREAM_NOT_READABLE,
                               std::move(read_cb_));
    }

    is_reading_ = true;
    read_cb_ = std::move(cb);
    external_read_buffer_ = out;
    read_message_size_ = bytes;
    reading_some_ = some;
    external_read_buffer_ = external_read_buffer_.first(read_message_size_);

    if (bytes_available_now > 0) {
      internal_read_buffer_.consume(external_read_buffer_);
    }
  }

  void YamuxStream::readCompleted() {
    if (is_reading_) {
      is_reading_ = false;
      size_t read_message_size = read_message_size_;
      read_message_size_ = 0;
      reading_some_ = false;
      if (read_cb_) {
        auto cb = std::move(read_cb_);
        read_cb_ = ReadCallbackFunc{};
        cb(read_message_size);
      }
    }
  }

  void YamuxStream::doWrite() {
    gsl::span<const uint8_t> data;
    bool some = false;
    while (!close_reason_) {
      send_window_size_ = write_queue_.dequeue(send_window_size_, data, some);
      if (data.empty()) {
        break;
      }
      feedback_.writeStreamData(stream_id_, data, some);
    }

    if (!is_writable_ && !close_reason_ && send_window_size_ > 0) {
      // closing stream for writes, sends FIN
      feedback_.streamClosed(stream_id_);

      if (!is_readable_) {
        doClose(YamuxError::STREAM_CLOSED_BY_HOST, false);
      } else {
        // let bytes be consumed with peers FIN even if no reader
        receive_window_size_ = maximum_window_size_;
      }
    }
  }

  void YamuxStream::doWrite(gsl::span<const uint8_t> in, size_t bytes,
                            WriteCallbackFunc cb, bool some) {
    if (bytes == 0 || in.empty() || static_cast<size_t>(in.size()) < bytes) {
      return deferWriteCallback(YamuxError::INVALID_ARGUMENT, std::move(cb));
    }

    if (!is_writable_) {
      return deferWriteCallback(YamuxError::STREAM_NOT_WRITABLE, std::move(cb));
    }

    if (close_reason_) {
      return deferWriteCallback(close_reason_, std::move(cb));
    }

    if (!write_queue_.canEnqueue(bytes)) {
      return deferWriteCallback(YamuxError::STREAM_WRITE_BUFFER_OVERFLOW,
                                std::move(cb));
    }

    write_queue_.enqueue(in.first(bytes), some, std::move(cb));
    doWrite();
  }

}  // namespace libp2p::connection
