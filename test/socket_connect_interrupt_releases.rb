# A blocking Socket#connect that SIGINT interrupts (Interrupt, in a program
# that starts no thread) leaves the socket closable: after the program
# rescues it and closes the socket, the descriptor is free, and the next
# socket is given the same number. The connect waits on a listener whose
# queue is full, which keeps it waiting on Linux; where the kernel completes
# it anyway (macOS), the signal lands in the sleep after it instead, and the
# output is the same.
require "socket"

srv = Socket.new(:INET, :STREAM)
srv.bind(Socket.sockaddr_in(0, "127.0.0.1"))
srv.listen(1)
port = Socket.unpack_sockaddr_in(srv.getsockname)[0]
fill = []
8.times do
  c = Socket.new(:INET, :STREAM)
  begin
    c.connect_nonblock(Socket.sockaddr_in(port, "127.0.0.1"))
  rescue IO::WaitWritable, Errno::EISCONN
  end
  fill << c
end

s = Socket.new(:INET, :STREAM)
fd = s.fileno
helper = Process.spawn("sh", "-c", "sleep 0.5; kill -INT #{Process.pid}")
interrupted = false
begin
  begin
    s.connect(Socket.sockaddr_in(port, "127.0.0.1"))
  rescue SystemCallError
  end
  sleep 5
rescue Interrupt
  interrupted = true
end
Process.wait(helper)
p interrupted
s.close
again = Socket.new(:INET, :STREAM)
p again.fileno == fd
again.close
fill.each(&:close)
srv.close
