# write_nonblock of an instance-variable String on a receiver that holds a
# user object or a socket writes each byte past a NUL: the socket got the
# bytes before the first NUL.
require "socket"

class Mem
  def write_nonblock(bytes, exception: true)
    bytes.bytesize
  end
end

class Holder
  def initialize(io)
    @io = io
    @output = "".b
  end

  def push(data)
    @output << data
  end

  def nb_false = @io.write_nonblock(@output, exception: false)
  def nb = @io.write_nonblock(@output)
  def nb_flag(flag) = @io.write_nonblock(@output, exception: flag)
end

server = TCPServer.new("127.0.0.1", 0)
client = TCPSocket.new("127.0.0.1", server.addr[1])
peer = server.accept
data = "ab\x00cd".b
m = Holder.new(Mem.new)
m.push(data)
p m.nb_false
p m.nb
p m.nb_flag(ARGV.empty?)

h = Holder.new(client)
h.push(data)
p h.nb_false
p peer.readpartial(100).bytesize
p h.nb
p peer.readpartial(100).bytesize
p h.nb_flag(ARGV.empty?)
p peer.readpartial(100).bytesize
client.close
peer.close
server.close
