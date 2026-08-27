# Socket#connect_nonblock 1-arg form: the packed sockaddr path. Mirrors
# the 2-arg shape: default raises IO::WaitWritable on EINPROGRESS;
# `exception: false` returns the :wait_writable symbol instead.
# Refusal (ECONNREFUSED) raises immediately regardless of the option,
# because refusal is a hard error, not "in progress".
require "socket"

# Pack a 16-byte sockaddr_in for 127.0.0.1:port. Addrinfo#to_sockaddr
# and Socket.sockaddr_in are not codegen-supported yet, so build the
# same layout the runtime reads: sa_family (LE) + sin_port (BE) +
# sin_addr + 8 zero bytes of padding.
def sockaddr_in(port, host)
  [Socket::AF_INET, port].pack("vn") +
    host.split(".").map(&:to_i).pack("C4") + ("\x00" * 8)
end

# 1) Refused port. ECONNREFUSED comes back synchronously because
#    nothing is listening. The exception class is independent of the
#    `exception` option - it is a hard error, not a "would block".
s1 = Socket.new(Socket::AF_INET, Socket::SOCK_STREAM, 0)
refused =
  begin
    s1.connect_nonblock(sockaddr_in(1, "127.0.0.1"))
  rescue => e
    e.class
  end
p refused
s1.close

# 2) Default mode on a live handshake. The first call has two
#    platform-dependent outcomes: it may return 0 (loopback connect
#    completed before the syscall returned) or raise IO::WaitWritable
#    (connect is queued). The test accepts either via rescue.
srv = TCPServer.new("127.0.0.1", 0)
port = srv.addr[1]
t = Thread.new { srv.accept }
s2 = Socket.new(Socket::AF_INET, Socket::SOCK_STREAM, 0)
default =
  begin
    s2.connect_nonblock(sockaddr_in(port, "127.0.0.1"))
    :ok
  rescue IO::WaitWritable
    :wait_writable
  end
p default
# Park until the handshake completes, then ask again. The result is
# 0 (kernel says the socket is connected and the syscall agrees) or
# Errno::EISCONN (kernel: you already tried this, here is the status).
# Both are valid "we are connected" answers in the default mode.
IO.select(nil, [s2])
begin
  s2.connect_nonblock(sockaddr_in(port, "127.0.0.1"))
  p :ok
rescue Errno::EISCONN
  p :eisconn
end
t.value.close rescue nil
srv.close
s2.close

# 3) `exception: false` on the same handshake path. The first call
#    returns the :wait_writable symbol or 0 (immediate success); the
#    second returns 0 because the no_exc path collapses EISCONN to
#    success instead of raising.
srv2 = TCPServer.new("127.0.0.1", 0)
port2 = srv2.addr[1]
t2 = Thread.new { srv2.accept }
s3 = Socket.new(Socket::AF_INET, Socket::SOCK_STREAM, 0)
flag = s3.connect_nonblock(sockaddr_in(port2, "127.0.0.1"), exception: false)
p flag
IO.select(nil, [s3])
again = s3.connect_nonblock(sockaddr_in(port2, "127.0.0.1"), exception: false)
p again
t2.value.close rescue nil
srv2.close
s3.close
