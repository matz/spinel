# A write to a pipe whose reader is gone raises Errno::EPIPE instead of
# ending the program with SIGPIPE: the runtime takes the signal with a
# handler that does nothing from the start, as CRuby does. A handler, not
# SIG_IGN, so a child process gets the default disposition back.
r, w = IO.pipe
r.close
begin
  w.write("x" * 70000)
rescue Errno::EPIPE => e
  puts "EPIPE #{e.class}"
end
w.close

# the handler is not inherited: a child killed by SIGPIPE dies of it
system("sh", "-c", "kill -PIPE $$")
p $?.termsig

# trap("PIPE", "DEFAULT") keeps the EPIPE behaviour, as CRuby's default
# handler of PIPE does nothing too
trap("PIPE", "DEFAULT")
r, w = IO.pipe
r.close
begin
  w.write("y")
rescue Errno::EPIPE => e
  puts "EPIPE again"
end
puts "alive"

# opening a socket changes nothing: the write still raises, and a child
# still gets the default
require "socket"
server = TCPServer.new("127.0.0.1", 0)
r, w = IO.pipe
r.close
begin
  w.write("z")
rescue Errno::EPIPE
  puts "EPIPE with a socket open"
end
system("sh", "-c", "kill -PIPE $$")
p $?.termsig
server.close
