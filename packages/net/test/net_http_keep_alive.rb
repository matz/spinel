# The requests of one `start` session share its connection while both sides
# keep it alive (#10809): a server that keeps it sees one connection for two
# requests. It is opened again when the server says `Connection: close`,
# when the server closed it while it sat idle, or after keep_alive_timeout;
# a request with no session asks for `Connection: close`. An idempotent
# request whose reused connection fails before the answer is sent again,
# once; a POST is not.
require "net/http"
require "socket"

# A server on its own thread. Each connection is handed to answer with its
# number (from 1) and the number of the request on it (from 1); answer
# returns :keep to read the next request, :close to close after answering
# with `Connection: close`, :quiet to close after answering without saying
# so, or :drop to close without answering. Every request's Connection header is
# kept in `conns`.
class KaServer
  attr_reader :port, :conns

  def initialize(&answer)
    @server = TCPServer.new("127.0.0.1", 0)
    @port = @server.addr[1]
    @accepts = 0
    @conns = []
    @mutex = Mutex.new
    @answer = answer
    @thread = Thread.new { serve }
  end

  def accepts = @mutex.synchronize { @accepts }

  def serve
    loop do
      socket = @server.accept
      n = @mutex.synchronize { @accepts += 1 }
      handle(socket, n)
    end
  rescue IOError
    nil
  end

  def handle(socket, n)
    i = 0
    loop do
      line = socket.gets
      break if line.nil?
      conn = ""
      length = 0
      while (h = socket.gets) && h != "\r\n"
        conn = h.split(":", 2)[1].strip if h.downcase.start_with?("connection:")
        length = h.split(":", 2)[1].strip.to_i if h.downcase.start_with?("content-length:")
      end
      socket.read(length) if length > 0
      @mutex.synchronize { @conns << conn }
      i += 1
      what = @answer.call(n, i)
      break if what == :drop
      body = "c#{n}r#{i}"
      socket.write "HTTP/1.1 200 OK\r\nContent-Length: #{body.bytesize}\r\n"
      socket.write "Connection: close\r\n" if what == :close
      socket.write "\r\n#{body}"
      break if what == :close || what == :quiet
    end
    socket.close
  end

  def stop
    @server.close
    @thread.join
  end
end

def run(server, timeout = nil)
  http = Net::HTTP.new("127.0.0.1", server.port)
  http.keep_alive_timeout = timeout unless timeout.nil?
  bodies = []
  http.start do |session|
    yield session, bodies
  end
  sleep 0.05
  server.stop
  puts "#{bodies.join(" ")} accepts=#{server.accepts} connection=#{server.conns.inspect}"
end

# two GETs, a server that keeps the connection: one connection
s = KaServer.new { |_n, _i| :keep }
run(s) { |h, b| 2.times { b << h.get("/").body } }

# the server says Connection: close after the first answer: a new one
s = KaServer.new { |_n, i| i == 1 ? :close : :keep }
run(s) { |h, b| 2.times { b << h.get("/").body } }

# the server closes the connection after answering without saying so; the
# client finds it closed before the next request, a POST, and opens another
s = KaServer.new { |n, _i| n == 1 ? :quiet : :keep }
run(s) do |h, b|
  b << h.get("/").body
  sleep 0.05
  b << h.post("/", "x").body
end

# keep_alive_timeout 0: an idle connection is never reused
s = KaServer.new { |_n, _i| :keep }
run(s, 0) { |h, b| 2.times { b << h.get("/").body } }

# a GET whose reused connection the server drops before answering is sent
# again on a new one, once
s = KaServer.new { |n, i| n == 1 && i == 2 ? :drop : :keep }
run(s) { |h, b| 2.times { b << h.get("/").body } }

# a POST there is not: EOFError
s = KaServer.new { |n, i| n == 1 && i == 2 ? :drop : :keep }
run(s) do |h, b|
  b << h.post("/", "x").body
  begin
    b << h.post("/", "y").body
  rescue EOFError => e
    b << e.class.name
  end
end

# a request with no session asks for Connection: close
s = KaServer.new { |_n, _i| :keep }
http = Net::HTTP.new("127.0.0.1", s.port)
res = http.request(Net::HTTP::Get.new("/"))
sleep 0.05
s.stop
puts "#{res.body} accepts=#{s.accepts} connection=#{s.conns.inspect}"

# max_retries: what it was set to, and a negative count refused
http = Net::HTTP.new("127.0.0.1", 1)
p http.max_retries
http.max_retries = 3
p http.max_retries
begin
  http.max_retries = -1
rescue ArgumentError => e
  puts e.message
end
