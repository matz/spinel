# write_nonblock on a File opened "r", the read end of a pipe or a socket
# after close_write raises IOError "not opened for writing", as write does.
# A handle open for writing still writes.
require "socket"
require "tmpdir"

def attempt
  yield
rescue IOError, SystemCallError => e
  "#{e.class}: #{e.message}"
end

dir = Dir.mktmpdir
path = File.join(dir, "ro.txt")
File.write(path, "abc")
f = File.open(path, "r")
p attempt { f.write_nonblock("x") }
p attempt { f.write_nonblock("x\0y".b) }
p attempt { f.write_nonblock([1, "z"][ARGV.size]) }
p attempt { f.write_nonblock(:sym) }
f.close

r, w = IO.pipe
p attempt { r.write_nonblock("x") }
p attempt { w.write_nonblock("x") }
r.close
w.close

a, b = UNIXSocket.pair
a.close_write
p attempt { a.write_nonblock("x") }
p attempt { b.write_nonblock("y") }
a.close
b.close

g = File.open(path, "r+")
p attempt { g.write_nonblock("q") }
g.close
p File.read(path)
File.delete(path)
Dir.rmdir(dir)
