# An exception must close the response socket before a following request.
# The control connection checks EOF before that following request can reconnect.
require "net/http"
require "socket"

def read_headers(socket)
  while (line = socket.gets)
    break if line == "\r\n"
  end
end

# A pool's private hook may distinguish a closed socket from no socket.
class CheckedTransportHTTP < Net::HTTP
  private

  def begin_transport(req)
    raise IOError, "missing socket in started session" if @socket.nil?
    connect if @socket.closed?
    super
  end
end

["request", "reader", "timeout", "eof"].each do |mode|
  server = TCPServer.new("127.0.0.1", 0)
  control = TCPServer.new("127.0.0.1", 0)
  thread = Thread.new do
    first = server.accept
    read_headers(first)
    if mode == "eof"
      first.shutdown(Socket::SHUT_WR)
    else
      first.write("HTTP/1.1 200 OK\r\nContent-Length: 8\r\n\r\npart")
    end
    ack = control.accept
    closed = false
    begin
      closed = !IO.select([first], nil, nil, 2).nil? && first.read(1).nil?
    rescue Errno::ECONNRESET
      closed = true
    end
    first.close
    ack.write(closed ? "1" : "0")
    ack.close
    second = server.accept
    read_headers(second)
    second.write("HTTP/1.1 200 OK\r\nContent-Length: 4\r\nConnection: close\r\n\r\nnext")
    second.close
  end
  http = CheckedTransportHTTP.new("127.0.0.1", server.addr[1], nil)
  http.max_retries = 0
  http.read_timeout = 5
  http.start
  begin
    http.request(Net::HTTP::Get.new("/first")) do |response|
      http.read_timeout = 0.2 if mode == "timeout"
      raise "consumer stopped" if mode == "request"
      response.read_body { |_chunk| raise "consumer stopped" } if mode == "reader"
    end
  rescue RuntimeError, Net::ReadTimeout, EOFError => error
    puts error.class
  end
  ack = TCPSocket.new("127.0.0.1", control.addr[1])
  p ack.read(1) == "1"
  ack.close
  p http.started?
  http.read_timeout = 5
  response = http.request(Net::HTTP::Get.new("/next"))
  p response.code
  p response.body == "next"
  http.finish
  p !http.started?
  thread.value
  server.close
  control.close
end

# A body reader can return early without raising. The peer closes its
# incomplete response; the next request must start on a fresh connection.
server = TCPServer.new("127.0.0.1", 0)
thread = Thread.new do
  first = server.accept
  read_headers(first)
  first.write("HTTP/1.1 200 OK\r\nContent-Length: 8\r\n\r\npart")
  first.close
  second = server.accept
  read_headers(second)
  second.write("HTTP/1.1 200 OK\r\nContent-Length: 4\r\nConnection: close\r\n\r\nnext")
  second.close
end
http = Net::HTTP.new("127.0.0.1", server.addr[1], nil)
http.read_timeout = 5
http.start do
  response = http.request(Net::HTTP::Get.new("/first")) do |res|
    res.read_body do |fragment|
      p !fragment.empty? && "part".start_with?(fragment)
      break
    end
  end
  p response.code
  response = http.request(Net::HTTP::Get.new("/next"))
  p response.code
  p response.body == "next"
end
thread.value
server.close

# Returning without reading the body must drain it and preserve keep-alive.
# Reading it fully must preserve the very same connection too.
[false, true].each do |read_body|
  server = TCPServer.new("127.0.0.1", 0)
  thread = Thread.new do
    socket = server.accept
    2.times do
      read_headers(socket)
      socket.write("HTTP/1.1 200 OK\r\nContent-Length: 4\r\n\r\nbody")
    end
    socket.close
  end
  http = Net::HTTP.new("127.0.0.1", server.addr[1], nil)
  http.read_timeout = 5
  http.start do
    response = http.request(Net::HTTP::Get.new("/first")) do |res|
      p res.body == "body" if read_body
    end
    p response.code
    p response.body == "body"
    response = http.request(Net::HTTP::Get.new("/next"))
    p response.code
    p response.body == "body"
  end
  thread.value
  server.close
end

# Explicitly fill Spinel's HTTP buffer before parsing the first response,
# even under short reads. CRuby's buffered socket needs no such setup.
class PrefetchedHTTP < Net::HTTP
  attr_writer :prefetch_bytes

  private

  def wire_gets
    if @prefetch_bytes > 0
      while @wire_buffer.bytesize < @prefetch_bytes
        chunk = wire_receive(@prefetch_bytes - @wire_buffer.bytesize)
        raise EOFError if chunk.nil?
        @wire_buffer << chunk
      end
      @prefetch_bytes = 0
    end
    super
  end
end

# Bytes left after a complete body still belong to a reused socket.
first_reply = "HTTP/1.1 200 OK\r\nContent-Length: 4\r\n\r\nbodypart"
server = TCPServer.new("127.0.0.1", 0)
thread = Thread.new do
  socket = server.accept
  read_headers(socket)
  socket.write(first_reply)
  read_headers(socket)
  socket.write("HTTP/1.1 200 OK\r\nContent-Length: 4\r\nConnection: close\r\n\r\nnext")
  socket.close
end
http = PrefetchedHTTP.new("127.0.0.1", server.addr[1], nil)
http.prefetch_bytes = first_reply.bytesize
http.read_timeout = 5
http.start do
  p http.request(Net::HTTP::Get.new("/first")).body == "body"
  begin
    p http.request(Net::HTTP::Get.new("/next")).code
  rescue Net::HTTPBadResponse => error
    puts error.class
  end
end
thread.value
server.close
