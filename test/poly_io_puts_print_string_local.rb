# puts and print of a String held in a variable, on a receiver known only at
# run time, in a program where a class defines puts(*args) and print(*args)
# (OpenSSL::Buffering does, so `require "openssl"` is enough). The rest
# parameter takes the String itself, so the dispatch holds the argument as
# its handle, and the arm for an IO known only at run time passed that handle
# where sp_File_puts_bin and sp_File_write_bin take the bytes: the C did not
# build.
require "stringio"

class Sock
  def initialize = (@buf = +"")
  def puts(*args)
    args.each { |a| @buf << a.to_s << "\n" }
    nil
  end
  def print(*args)
    args.each { |a| @buf << a.to_s }
    nil
  end
  def buf = @buf
end

class Lg
  def initialize(io) = (@io = io)
  def log(line)
    @io.puts(line) if @io
    nil
  end
  def raw(line)
    @io.print(line) if @io
    nil
  end
  def pair(a, b)
    @io.puts(a, b) if @io
    @io.print(a, b, "\n") if @io
    nil
  end
end

# no io at all: nothing is written
Lg.new(nil).log("hello")
Lg.new(nil).raw("hello")
Lg.new(nil).pair("a", "b")
puts "ok"

out = Lg.new($stdout)
out.log("to stdout")
out.log("ends in a newline\n")
out.raw("raw to stdout\n")
out.pair("a", "b")

sio = StringIO.new
mem = Lg.new(sio)
mem.log("to a StringIO")
mem.raw("raw")
mem.pair("c", "d")
p sio.string

sock = Sock.new
own = Lg.new(sock)
own.log("to its own puts")
own.raw("raw")
own.pair("e", "f")
p sock.buf

# the String the variable holds when the call runs
s = +"grown"
s << "!"
out.log(s)
out.raw(s)
out.raw("\n")

# a receiver narrowed to an IO
def narrowed(io, line)
  return unless io.is_a?(IO)
  io.puts(line)
  io.print(line, "\n")
end
narrowed($stdout, "narrowed")
narrowed(sock, "not an IO")
narrowed(nil, "nil")

# a value with no puts of its own
begin
  Lg.new(Object.new).log("x")
rescue NoMethodError => e
  puts e.class
end
begin
  Lg.new(Object.new).raw("x")
rescue NoMethodError => e
  puts e.class
end
