# spinel: not-cruby -- Uses net-protocol 0.4.0; CRuby 4.0 bundles 0.2.2.
# Expected output comes from CRuby with ruby/net-protocol v0.4.0 on its load path.
require "net/protocol"

class FragmentReader
  def initialize(chunks)
    @chunks = chunks.map(&:b)
  end

  def read_nonblock(size, buffer = nil, exception: false)
    chunk = @chunks.shift
    return nil if chunk.nil?
    if buffer
      buffer.replace(chunk)
    else
      chunk
    end
  end
end

io = Net::BufferedIO.new(FragmentReader.new(["abc\r", "\ndef\r\n"]))
p io.readuntil("\r\n", limit: 5)
p io.readuntil("\r\n", limit: 5)

io = Net::BufferedIO.new(FragmentReader.new(["a", "\r", "\n", "\r", "\n"]))
p io.readuntil("\r\n\r\n")

io = Net::BufferedIO.new(FragmentReader.new(["ab\n"]))
p io.readuntil("ab")

# Search cannot rewind into a prefix returned by the earlier read.
io = Net::BufferedIO.new(FragmentReader.new(["ab\r\n\r", "\nc"]))
p io.readuntil("\r")
begin
  io.readuntil("\r\n\r\n")
rescue => error
  p error.class
end
p io.readuntil("\r\n\r\n", true)

io = Net::BufferedIO.new(FragmentReader.new(["long prefix\nb", "c\n"]))
p io.readuntil("\n", limit: 12)
p io.readuntil("\n", limit: 3)

io = Net::BufferedIO.new(FragmentReader.new(["abc\r", "\nnext\r\n"]))
begin
  io.readuntil("\r\n", limit: 4)
rescue => error
  p error.class
end
p io.readuntil("\r\n", limit: 5)
p io.readuntil("\r\n", limit: 6)

io = Net::BufferedIO.new(FragmentReader.new(["abcdef"]))
p io.read(1)
begin
  io.read(-1)
rescue => error
  p error.class
end
p io.read_all

# Bound the synthetic producer too: a lost limit must fail, not hang.
class RepeatingReader
  attr_reader :reads

  def initialize
    @reads = 0
  end

  def read_nonblock(size, buffer = nil, exception: false)
    @reads += 1
    raise "too many reads" if @reads > 3
    chunk = "x" * size
    buffer ? buffer.replace(chunk) : chunk
  end
end

reader = RepeatingReader.new
io = Net::BufferedIO.new(reader)
begin
  io.readuntil("\n", limit: 8)
rescue => error
  p error.class
end
p reader.reads
