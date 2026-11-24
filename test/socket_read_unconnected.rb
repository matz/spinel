# Reads on a socket that was never connected raise Errno::ENOTCONN, and
# reads on a directory raise Errno::EISDIR, where the stdio reads took the
# read error for the end of the stream.
require "socket"
require "tmpdir"
require "open3"

if ARGV[0] == "stdin"
  p(($stdin.gets rescue $!))
  p(($stdin.read rescue $!))
  p(($stdin.getc rescue $!))
  exit
end

def show(label, r)
  if r.is_a?(Exception)
    m = r.message.sub(Errno::ENOTCONN.new.message, "<ENOTCONN>").sub(/fd:\d+/, "fd:N")
    puts "#{label}: #{r.class}: #{m.inspect}"
  else
    puts "#{label}: #{r.inspect}"
  end
end

u = Socket.new(Socket::AF_INET, Socket::SOCK_STREAM)
show("gets", (u.gets rescue $!))
show("read", (u.read rescue $!))
show("readline", (u.readline rescue $!))
show("getc", (u.getc rescue $!))
show("getbyte", (u.getbyte rescue $!))
show("read(10)", (u.read(10) rescue $!))
show("readchar", (u.readchar rescue $!))
show("readbyte", (u.readbyte rescue $!))
n = 0
show("each_line", (u.each_line { n += 1 } rescue $!))
puts "lines: #{n}"
show("readlines", (u.readlines rescue $!))
show("gets again", (u.gets rescue $!))
u.close

d = File.open(Dir.tmpdir)
show("dir gets", (d.gets rescue $!).class)
show("dir getc", (d.getc rescue $!).class)
show("dir read(10)", (d.read(10) rescue $!).class)
d.close

# a timed read's raise releases the handle, so the close frees its descriptor
d = File.open(Dir.tmpdir)
fd = d.fileno
d.timeout = 1
show("dir timed read(10)", (d.read(10) rescue $!).class)
d.close
f = File.open(__FILE__)
puts "fd after close: #{f.fileno == fd ? "reused" : "leaked"}"
f.close

# a closed fd 0 reads as the end of the stream
ruby = RUBY_ENGINE == "spinel" ? "" : "ruby "
out = Open3.capture2("sh", "-c", "exec #{ruby}'#{$0}' stdin 0<&-")[0]
puts "closed stdin:", out
