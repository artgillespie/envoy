#include <cstdint>
#include <memory>
#include <string>

#include "common/buffer/buffer_impl.h"
#include "common/common/empty_string.h"
#include "common/event/dispatcher_impl.h"
#include "common/network/address_impl.h"
#include "common/network/connection_impl.h"
#include "common/network/listen_socket_impl.h"
#include "common/network/utility.h"
#include "common/runtime/runtime_impl.h"
#include "common/stats/stats_impl.h"

#include "test/mocks/buffer/mocks.h"
#include "test/mocks/network/mocks.h"
#include "test/mocks/server/mocks.h"
#include "test/mocks/stats/mocks.h"
#include "test/test_common/environment.h"
#include "test/test_common/network_utility.h"
#include "test/test_common/printers.h"
#include "test/test_common/utility.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace Envoy {
using testing::AnyNumber;
using testing::DoAll;
using testing::InSequence;
using testing::Invoke;
using testing::Return;
using testing::Sequence;
using testing::StrictMock;
using testing::Test;
using testing::_;

namespace Network {

TEST(ConnectionImplUtility, updateBufferStats) {
  StrictMock<Stats::MockCounter> counter;
  StrictMock<Stats::MockGauge> gauge;
  uint64_t previous_total = 0;

  InSequence s;
  EXPECT_CALL(counter, add(5));
  EXPECT_CALL(gauge, add(5));
  ConnectionImplUtility::updateBufferStats(5, 5, previous_total, counter, gauge);
  EXPECT_EQ(5UL, previous_total);

  EXPECT_CALL(counter, add(1));
  EXPECT_CALL(gauge, sub(1));
  ConnectionImplUtility::updateBufferStats(1, 4, previous_total, counter, gauge);

  EXPECT_CALL(gauge, sub(4));
  ConnectionImplUtility::updateBufferStats(0, 0, previous_total, counter, gauge);

  EXPECT_CALL(counter, add(3));
  EXPECT_CALL(gauge, add(3));
  ConnectionImplUtility::updateBufferStats(3, 3, previous_total, counter, gauge);
}

class ConnectionImplDeathTest : public testing::TestWithParam<Address::IpVersion> {};
INSTANTIATE_TEST_CASE_P(IpVersions, ConnectionImplDeathTest,
                        testing::ValuesIn(TestEnvironment::getIpVersionsForTest()));

TEST_P(ConnectionImplDeathTest, BadFd) {
  Event::DispatcherImpl dispatcher;
  EXPECT_DEATH(ConnectionImpl(dispatcher, -1,
                              Network::Test::getCanonicalLoopbackAddress(GetParam()),
                              Network::Test::getCanonicalLoopbackAddress(GetParam())),
               ".*assert failure: fd_ != -1.*");
}

class ConnectionImplTest : public testing::TestWithParam<Address::IpVersion> {
public:
  void setUpBasicConnection() {
    if (dispatcher_.get() == nullptr) {
      dispatcher_.reset(new Event::DispatcherImpl);
    }
    listener_ =
        dispatcher_->createListener(connection_handler_, socket_, listener_callbacks_, stats_store_,
                                    Network::ListenerOptions::listenerOptionsWithBindToPort());

    client_connection_ = dispatcher_->createClientConnection(socket_.localAddress());
    client_connection_->addConnectionCallbacks(client_callbacks_);
  }

  void connect() {
    client_connection_->connect();
    read_filter_.reset(new NiceMock<MockReadFilter>());
    EXPECT_CALL(listener_callbacks_, onNewConnection_(_))
        .WillOnce(Invoke([&](Network::ConnectionPtr& conn) -> void {
          server_connection_ = std::move(conn);
          server_connection_->addConnectionCallbacks(server_callbacks_);
          server_connection_->addReadFilter(read_filter_);
        }));
    EXPECT_CALL(client_callbacks_, onEvent(ConnectionEvent::Connected));
    dispatcher_->run(Event::Dispatcher::RunType::NonBlock);
  }

  void disconnect() {
    EXPECT_CALL(client_callbacks_, onEvent(ConnectionEvent::LocalClose));
    client_connection_->close(ConnectionCloseType::NoFlush);
    dispatcher_->run(Event::Dispatcher::RunType::NonBlock);
  }

  void useMockBuffer() {
    // This needs to be called before the dispatcher is created.
    ASSERT(dispatcher_.get() == nullptr);

    MockBufferFactory* factory = new StrictMock<MockBufferFactory>;
    dispatcher_.reset(new Event::DispatcherImpl(Buffer::FactoryPtr{factory}));
    // The first call to create a client session will get a MockBuffer.
    // Other calls for server sessions will by default get a normal OwnedImpl.
    EXPECT_CALL(*factory, create_())
        .Times(AnyNumber())
        .WillOnce(Invoke([&]() -> Buffer::Instance* {
          return new MockBuffer; // client read buffer.
        }))
        .WillOnce(Invoke([&]() -> Buffer::Instance* {
          client_write_buffer_ = new StrictMock<MockBuffer>;
          return client_write_buffer_;
        }))
        .WillRepeatedly(Invoke([]() -> Buffer::Instance* { return new Buffer::OwnedImpl; }));
  }

protected:
  Event::DispatcherPtr dispatcher_;
  Stats::IsolatedStoreImpl stats_store_;
  Network::TcpListenSocket socket_{Network::Test::getAnyAddress(GetParam()), true};
  Network::MockListenerCallbacks listener_callbacks_;
  Network::MockConnectionHandler connection_handler_;
  Network::ListenerPtr listener_;
  Network::ClientConnectionPtr client_connection_;
  StrictMock<MockConnectionCallbacks> client_callbacks_;
  Network::ConnectionPtr server_connection_;
  StrictMock<Network::MockConnectionCallbacks> server_callbacks_;
  std::shared_ptr<MockReadFilter> read_filter_;
  MockBuffer* client_write_buffer_ = nullptr;
};

INSTANTIATE_TEST_CASE_P(IpVersions, ConnectionImplTest,
                        testing::ValuesIn(TestEnvironment::getIpVersionsForTest()));

TEST_P(ConnectionImplTest, CloseDuringConnectCallback) {
  setUpBasicConnection();

  Buffer::OwnedImpl buffer("hello world");
  client_connection_->write(buffer);
  client_connection_->connect();

  EXPECT_CALL(client_callbacks_, onEvent(ConnectionEvent::Connected))
      .WillOnce(Invoke(
          [&](uint32_t) -> void { client_connection_->close(ConnectionCloseType::NoFlush); }));
  EXPECT_CALL(client_callbacks_, onEvent(ConnectionEvent::LocalClose));

  read_filter_.reset(new NiceMock<MockReadFilter>());
  EXPECT_CALL(listener_callbacks_, onNewConnection_(_))
      .WillOnce(Invoke([&](Network::ConnectionPtr& conn) -> void {
        server_connection_ = std::move(conn);
        server_connection_->addConnectionCallbacks(server_callbacks_);
        server_connection_->addReadFilter(read_filter_);
      }));

  EXPECT_CALL(server_callbacks_, onEvent(ConnectionEvent::RemoteClose))
      .WillOnce(Invoke([&](uint32_t) -> void { dispatcher_->exit(); }));

  dispatcher_->run(Event::Dispatcher::RunType::Block);
}

struct MockBufferStats {
  Connection::BufferStats toBufferStats() {
    return {rx_total_, rx_current_, tx_total_, tx_current_};
  }

  StrictMock<Stats::MockCounter> rx_total_;
  StrictMock<Stats::MockGauge> rx_current_;
  StrictMock<Stats::MockCounter> tx_total_;
  StrictMock<Stats::MockGauge> tx_current_;
};

TEST_P(ConnectionImplTest, BufferStats) {
  setUpBasicConnection();

  MockBufferStats client_buffer_stats;
  client_connection_->setBufferStats(client_buffer_stats.toBufferStats());
  client_connection_->connect();

  std::shared_ptr<MockWriteFilter> write_filter(new MockWriteFilter());
  std::shared_ptr<MockFilter> filter(new MockFilter());
  client_connection_->addWriteFilter(write_filter);
  client_connection_->addFilter(filter);

  Sequence s1;
  EXPECT_CALL(*write_filter, onWrite(_))
      .InSequence(s1)
      .WillOnce(Return(FilterStatus::StopIteration));
  EXPECT_CALL(*write_filter, onWrite(_)).InSequence(s1).WillOnce(Return(FilterStatus::Continue));
  EXPECT_CALL(*filter, onWrite(_)).InSequence(s1).WillOnce(Return(FilterStatus::Continue));
  EXPECT_CALL(client_callbacks_, onEvent(ConnectionEvent::Connected)).InSequence(s1);
  EXPECT_CALL(client_buffer_stats.tx_total_, add(4)).InSequence(s1);

  read_filter_.reset(new NiceMock<MockReadFilter>());
  MockBufferStats server_buffer_stats;
  EXPECT_CALL(listener_callbacks_, onNewConnection_(_))
      .WillOnce(Invoke([&](Network::ConnectionPtr& conn) -> void {
        server_connection_ = std::move(conn);
        server_connection_->addConnectionCallbacks(server_callbacks_);
        server_connection_->setBufferStats(server_buffer_stats.toBufferStats());
        server_connection_->addReadFilter(read_filter_);
        EXPECT_EQ("", server_connection_->nextProtocol());
      }));

  Sequence s2;
  EXPECT_CALL(server_buffer_stats.rx_total_, add(4)).InSequence(s2);
  EXPECT_CALL(server_buffer_stats.rx_current_, add(4)).InSequence(s2);
  EXPECT_CALL(server_buffer_stats.rx_current_, sub(4)).InSequence(s2);
  EXPECT_CALL(server_callbacks_, onEvent(ConnectionEvent::LocalClose)).InSequence(s2);

  EXPECT_CALL(*read_filter_, onNewConnection());
  EXPECT_CALL(*read_filter_, onData(_))
      .WillOnce(Invoke([&](Buffer::Instance& data) -> FilterStatus {
        data.drain(data.length());
        server_connection_->close(ConnectionCloseType::FlushWrite);
        return FilterStatus::StopIteration;
      }));

  EXPECT_CALL(client_callbacks_, onEvent(ConnectionEvent::RemoteClose))
      .WillOnce(Invoke([&](uint32_t) -> void { dispatcher_->exit(); }));

  Buffer::OwnedImpl data("1234");
  client_connection_->write(data);
  client_connection_->write(data);
  dispatcher_->run(Event::Dispatcher::RunType::Block);
}

// Ensure the new counter logic in ReadDisable avoids tripping asserts in ReadDisable guarding
// against actual enabling twice in a row.
TEST_P(ConnectionImplTest, ReadDisable) {
  setUpBasicConnection();

  client_connection_->readDisable(true);
  client_connection_->readDisable(false);

  client_connection_->readDisable(true);
  client_connection_->readDisable(true);
  client_connection_->readDisable(false);
  client_connection_->readDisable(false);

  client_connection_->readDisable(true);
  client_connection_->readDisable(true);
  client_connection_->readDisable(false);
  client_connection_->readDisable(true);
  client_connection_->readDisable(false);
  client_connection_->readDisable(false);

  disconnect();
}

// Test that as watermark levels are changed, the appropriate callbacks are triggered.
TEST_P(ConnectionImplTest, Watermarks) {
  useMockBuffer();

  setUpBasicConnection();

  // Stick 5 bytes in the connection buffer.
  std::unique_ptr<Buffer::OwnedImpl> buffer(new Buffer::OwnedImpl("hello"));
  int buffer_len = buffer->length();
  EXPECT_CALL(*client_write_buffer_, write(_))
      .WillOnce(Invoke(client_write_buffer_, &MockBuffer::failWrite));
  EXPECT_CALL(*client_write_buffer_, move(_));
  client_write_buffer_->move(*buffer);

  {
    // Go from watermarks being off to being above the high watermark.
    EXPECT_CALL(client_callbacks_, onAboveWriteBufferHighWatermark());
    EXPECT_CALL(client_callbacks_, onBelowWriteBufferLowWatermark()).Times(0);
    client_connection_->setBufferLimits(buffer_len - 3);
  }

  {
    // Go from above the high watermark to in between both.
    EXPECT_CALL(client_callbacks_, onAboveWriteBufferHighWatermark()).Times(0);
    EXPECT_CALL(client_callbacks_, onBelowWriteBufferLowWatermark()).Times(0);
    client_connection_->setBufferLimits(buffer_len + 1);
  }

  {
    // Go from above the high watermark to below the low watermark.
    EXPECT_CALL(client_callbacks_, onAboveWriteBufferHighWatermark()).Times(0);
    EXPECT_CALL(client_callbacks_, onBelowWriteBufferLowWatermark());
    client_connection_->setBufferLimits(buffer_len * 3);
  }

  {
    // Go back in between and verify neither callback is called.
    EXPECT_CALL(client_callbacks_, onAboveWriteBufferHighWatermark()).Times(0);
    EXPECT_CALL(client_callbacks_, onBelowWriteBufferLowWatermark()).Times(0);
    client_connection_->setBufferLimits(buffer_len * 2);
  }

  disconnect();
}

// Write some data to the connection.  It will automatically attempt to flush
// it to the upstream file descriptor via a write() call to buffer_, which is
// configured to succeed and accept all bytes read.
TEST_P(ConnectionImplTest, BasicWrite) {
  useMockBuffer();

  setUpBasicConnection();

  connect();

  // Send the data to the connection and verify it is sent upstream.
  std::string data_to_write = "hello world";
  Buffer::OwnedImpl buffer_to_write(data_to_write);
  std::string data_written;
  EXPECT_CALL(*client_write_buffer_, move(_))
      .WillRepeatedly(DoAll(AddBufferToStringWithoutDraining(&data_written),
                            Invoke(client_write_buffer_, &MockBuffer::baseMove)));
  EXPECT_CALL(*client_write_buffer_, write(_))
      .WillOnce(Invoke(client_write_buffer_, &MockBuffer::trackWrites));
  client_connection_->write(buffer_to_write);
  dispatcher_->run(Event::Dispatcher::RunType::NonBlock);
  EXPECT_EQ(data_to_write, data_written);

  EXPECT_CALL(server_callbacks_, onEvent(ConnectionEvent::RemoteClose));
  disconnect();
}

// Similar to BasicWrite, only with watermarks set.
TEST_P(ConnectionImplTest, WriteWithWatermarks) {
  useMockBuffer();

  setUpBasicConnection();

  connect();

  client_connection_->setBufferLimits(2);

  std::string data_to_write = "hello world";
  Buffer::OwnedImpl first_buffer_to_write(data_to_write);
  std::string data_written;
  EXPECT_CALL(*client_write_buffer_, move(_))
      .WillRepeatedly(DoAll(AddBufferToStringWithoutDraining(&data_written),
                            Invoke(client_write_buffer_, &MockBuffer::baseMove)));
  EXPECT_CALL(*client_write_buffer_, write(_))
      .WillOnce(Invoke(client_write_buffer_, &MockBuffer::trackWrites));
  // The write() call on the connection will buffer enough data to bring the connection above the
  // high watermark but the subsequent drain immediately brings it back below.
  // A nice future performance optimization would be to latch if the socket is writable in the
  // connection_impl, and try an immediate drain inside of write() to avoid thrashing here.
  EXPECT_CALL(client_callbacks_, onAboveWriteBufferHighWatermark());
  EXPECT_CALL(client_callbacks_, onBelowWriteBufferLowWatermark());
  client_connection_->write(first_buffer_to_write);
  dispatcher_->run(Event::Dispatcher::RunType::NonBlock);
  EXPECT_EQ(data_to_write, data_written);

  // Now do the write again, but this time configure buffer_ to reject the write
  // with errno set to EAGAIN via failWrite().  This should result in going above the high
  // watermark and not returning.
  Buffer::OwnedImpl second_buffer_to_write(data_to_write);
  EXPECT_CALL(*client_write_buffer_, move(_))
      .WillRepeatedly(DoAll(AddBufferToStringWithoutDraining(&data_written),
                            Invoke(client_write_buffer_, &MockBuffer::baseMove)));
  EXPECT_CALL(*client_write_buffer_, write(_))
      .WillOnce(Invoke(client_write_buffer_, &MockBuffer::failWrite));
  // The write() call on the connection will buffer enough data to bring the connection above the
  // high watermark and as the data will not flush it should not return below the watermark.
  EXPECT_CALL(client_callbacks_, onAboveWriteBufferHighWatermark());
  EXPECT_CALL(client_callbacks_, onBelowWriteBufferLowWatermark()).Times(0);
  client_connection_->write(second_buffer_to_write);
  dispatcher_->run(Event::Dispatcher::RunType::NonBlock);

  // Clean up the connection.  The close() will attempt to flush.  The call to
  // write() will succeed, bringing the connection back under the low watermark.
  EXPECT_CALL(client_callbacks_, onEvent(ConnectionEvent::LocalClose));
  EXPECT_CALL(*client_write_buffer_, write(_))
      .WillOnce(Invoke(client_write_buffer_, &MockBuffer::trackWrites));
  EXPECT_CALL(client_callbacks_, onBelowWriteBufferLowWatermark()).Times(1);
  EXPECT_CALL(server_callbacks_, onEvent(ConnectionEvent::RemoteClose));
  client_connection_->close(ConnectionCloseType::NoFlush);
  dispatcher_->run(Event::Dispatcher::RunType::NonBlock);
}

// Read and write random bytes and ensure we don't encounter issues.
TEST_P(ConnectionImplTest, WatermarkFuzzing) {
  useMockBuffer();
  setUpBasicConnection();

  connect();
  client_connection_->setBufferLimits(10);

  TestRandomGenerator rand;
  int bytes_buffered = 0;
  int new_bytes_buffered = 0;

  bool is_below = true;
  bool is_above = false;

  ON_CALL(*client_write_buffer_, write(_))
      .WillByDefault(testing::Invoke(client_write_buffer_, &MockBuffer::failWrite));
  ON_CALL(*client_write_buffer_, drain(_))
      .WillByDefault(testing::Invoke(client_write_buffer_, &MockBuffer::baseDrain));
  EXPECT_CALL(*client_write_buffer_, drain(_)).Times(AnyNumber());

  // Randomly write 1-20 bytes and read 1-30 bytes per loop.
  for (int i = 0; i < 50; ++i) {
    // The bytes to read this loop.
    int bytes_to_write = rand.random() % 20 + 1;
    // The bytes buffered at the begining of this loop.
    bytes_buffered = new_bytes_buffered;
    // Bytes to flush upstream.
    int bytes_to_flush = std::min<int>(rand.random() % 30 + 1, bytes_to_write + bytes_buffered);
    // The number of bytes buffered at the end of this loop.
    new_bytes_buffered = bytes_buffered + bytes_to_write - bytes_to_flush;
    ENVOY_LOG_MISC(trace,
                   "Loop iteration {} bytes_to_write {} bytes_to_flush {} bytes_buffered is {} and "
                   "will be be {}",
                   i, bytes_to_write, bytes_to_flush, bytes_buffered, new_bytes_buffered);

    std::string data(bytes_to_write, 'a');
    Buffer::OwnedImpl buffer_to_write(data);

    // If the current bytes buffered plus the bytes we write this loop go over
    // the watermark and we're not currently above, we will get a callback for
    // going above.
    if (bytes_to_write + bytes_buffered > 11 && is_below) {
      ENVOY_LOG_MISC(trace, "Expect onAboveWriteBufferHighWatermark");
      EXPECT_CALL(client_callbacks_, onAboveWriteBufferHighWatermark());
      is_below = false;
      is_above = true;
    }
    // If after the bytes are flushed upstream the number of bytes remaining is
    // below the low watermark and the bytes were not previously below the low
    // watermark, expect the callback for going below.
    if (new_bytes_buffered < 5 && is_above) {
      ENVOY_LOG_MISC(trace, "Expect onBelowWriteBufferLowWatermark");
      EXPECT_CALL(client_callbacks_, onBelowWriteBufferLowWatermark());
      is_below = true;
      is_above = false;
    }

    // Do the actual work.  Write |buffer_to_write| bytes to the connection and
    // drain |bytes_to_flush| before having the buffer failWrite()
    EXPECT_CALL(*client_write_buffer_, move(_))
        .WillOnce(Invoke(client_write_buffer_, &MockBuffer::baseMove));
    EXPECT_CALL(*client_write_buffer_, write(_))
        .WillOnce(DoAll(Invoke([&](int) -> void { client_write_buffer_->drain(bytes_to_flush); }),
                        Return(bytes_to_flush)))
        .WillRepeatedly(testing::Invoke(client_write_buffer_, &MockBuffer::failWrite));
    client_connection_->write(buffer_to_write);
    dispatcher_->run(Event::Dispatcher::RunType::NonBlock);
  }

  EXPECT_CALL(server_callbacks_, onEvent(_));
  disconnect();
}

class ReadBufferLimitTest : public ConnectionImplTest {
public:
  void readBufferLimitTest(uint32_t read_buffer_limit, uint32_t expected_chunk_size) {
    const uint32_t buffer_size = 256 * 1024;
    dispatcher_.reset(new Event::DispatcherImpl);
    listener_ =
        dispatcher_->createListener(connection_handler_, socket_, listener_callbacks_, stats_store_,
                                    {.bind_to_port_ = true,
                                     .use_proxy_proto_ = false,
                                     .use_original_dst_ = false,
                                     .per_connection_buffer_limit_bytes_ = read_buffer_limit});

    client_connection_ = dispatcher_->createClientConnection(socket_.localAddress());
    client_connection_->connect();

    read_filter_.reset(new NiceMock<MockReadFilter>());
    EXPECT_CALL(listener_callbacks_, onNewConnection_(_))
        .WillOnce(Invoke([&](Network::ConnectionPtr& conn) -> void {
          server_connection_ = std::move(conn);
          server_connection_->addReadFilter(read_filter_);
          EXPECT_EQ("", server_connection_->nextProtocol());
          EXPECT_EQ(read_buffer_limit, server_connection_->bufferLimit());
        }));

    uint32_t filter_seen = 0;

    EXPECT_CALL(*read_filter_, onNewConnection());
    EXPECT_CALL(*read_filter_, onData(_))
        .WillRepeatedly(Invoke([&](Buffer::Instance& data) -> FilterStatus {
          EXPECT_EQ(expected_chunk_size, data.length());
          filter_seen += data.length();
          data.drain(data.length());
          if (filter_seen == buffer_size) {
            server_connection_->close(ConnectionCloseType::FlushWrite);
          }
          return FilterStatus::StopIteration;
        }));

    client_connection_->addConnectionCallbacks(client_callbacks_);
    EXPECT_CALL(client_callbacks_, onEvent(ConnectionEvent::Connected));
    EXPECT_CALL(client_callbacks_, onEvent(ConnectionEvent::RemoteClose))
        .WillOnce(Invoke([&](uint32_t) -> void {
          EXPECT_EQ(buffer_size, filter_seen);
          dispatcher_->exit();
        }));

    Buffer::OwnedImpl data(std::string(buffer_size, 'a'));
    client_connection_->write(data);
    dispatcher_->run(Event::Dispatcher::RunType::Block);
  }
};

INSTANTIATE_TEST_CASE_P(IpVersions, ReadBufferLimitTest,
                        testing::ValuesIn(TestEnvironment::getIpVersionsForTest()));

TEST_P(ReadBufferLimitTest, NoLimit) { readBufferLimitTest(0, 256 * 1024); }

TEST_P(ReadBufferLimitTest, SomeLimit) { readBufferLimitTest(32 * 1024, 32 * 1024); }

class TcpClientConnectionImplTest : public testing::TestWithParam<Address::IpVersion> {};
INSTANTIATE_TEST_CASE_P(IpVersions, TcpClientConnectionImplTest,
                        testing::ValuesIn(TestEnvironment::getIpVersionsForTest()));

TEST_P(TcpClientConnectionImplTest, BadConnectNotConnRefused) {
  Event::DispatcherImpl dispatcher;
  Address::InstanceConstSharedPtr address;
  if (GetParam() == Network::Address::IpVersion::v4) {
    // Connecting to 255.255.255.255 will cause a perm error and not ECONNREFUSED which is a
    // different path in libevent. Make sure this doesn't crash.
    address = Utility::resolveUrl("tcp://255.255.255.255:1");
  } else {
    // IPv6 reserved multicast address.
    address = Utility::resolveUrl("tcp://[ff00::]:1");
  }
  ClientConnectionPtr connection = dispatcher.createClientConnection(address);
  connection->connect();
  connection->noDelay(true);
  dispatcher.run(Event::Dispatcher::RunType::Block);
}

TEST_P(TcpClientConnectionImplTest, BadConnectConnRefused) {
  Event::DispatcherImpl dispatcher;
  // Connecting to an invalid port on localhost will cause ECONNREFUSED which is a different code
  // path from other errors. Test this also.
  ClientConnectionPtr connection = dispatcher.createClientConnection(Utility::resolveUrl(
      fmt::format("tcp://{}:1", Network::Test::getLoopbackAddressUrlString(GetParam()))));
  connection->connect();
  connection->noDelay(true);
  dispatcher.run(Event::Dispatcher::RunType::Block);
}

} // namespace Network
} // namespace Envoy
