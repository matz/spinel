# spinel: gc-stress
# A large write to a Unix socket peer that reads in small pieces parks each
# time the send buffer fills and finishes, while the readers allocate. On
# macOS the write slept in the kernel and a collection waited on its worker.
require "socket"
pairs = Array.new(4) { Socket.pair(Socket::AF_UNIX, Socket::SOCK_STREAM, 0) }
chunk = "x" * (1024 * 1024)
writers = pairs.map do |left, _|
  Thread.new do
    3.times { left.write(chunk) }
    left.close
  end
end
readers = pairs.map do |_, right|
  Thread.new do
    got = 0
    junk = []
    while (part = right.readpartial(3000) rescue nil)
      got += part.bytesize
      junk << "alloc #{got}" * 8
      junk.clear if junk.size > 500
    end
    got
  end
end
writers.each(&:join)
p readers.map(&:value)
