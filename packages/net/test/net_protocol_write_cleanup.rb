# spinel: not-cruby -- Uses net-protocol 0.4.0; CRuby 4.0 bundles 0.2.2.
# Expected output comes from CRuby with ruby/net-protocol v0.4.0 on its load path.
require "net/protocol"

# An SSL write may need to read before it can make progress.
class ReadBeforeWrite
  attr_reader :output, :waits

  def initialize(ready)
    @ready = ready
    @waits = 0
    @writes = 0
    @output = "".b
  end

  def to_io
    self
  end

  def wait_readable(timeout)
    @waits += 1
    @ready
  end

  def write_nonblock(data, exception: false)
    @writes += 1
    raise "too many writes" if @writes > 3
    return :wait_readable if @waits == 0
    @output << data
    data.bytesize
  end
end

class ObservedBufferedIO < Net::BufferedIO
  attr_reader :written_bytes
end

transport = ReadBeforeWrite.new(true)
io = ObservedBufferedIO.new(transport)
p io.write("hello")
p transport.output, transport.waits, io.written_bytes

transport = ReadBeforeWrite.new(false)
io = ObservedBufferedIO.new(transport)
begin
  io.write("hello")
rescue => error
  p error.class
end
p transport.output, transport.waits, io.written_bytes

class ObservedMessageIO < Net::InternetMessageIO
  attr_reader :wbuf, :written_bytes
end

reader, writer = IO.pipe
debug = "".dup
io = ObservedMessageIO.new(writer, debug_output: debug)
begin
  io.write_message_by_block do |dest|
    dest.write("partial")
    raise "stop"
  end
rescue => error
  p error.class
end
p io.debug_output.equal?(debug), io.wbuf, io.written_bytes
p io.write_message("ok\n")
writer.close
p reader.read
reader.close

class FailingLines
  def each
    yield "partial"
    raise "stop"
  end
end

reader, writer = IO.pipe
debug = "".dup
io = ObservedMessageIO.new(writer, debug_output: debug)
begin
  io.write_message(FailingLines.new)
rescue => error
  p error.class
end
p io.debug_output.equal?(debug), io.wbuf, io.written_bytes
writer.close
p reader.read
reader.close

reader, writer = IO.pipe
writer.write("line\r\n.\r\n")
writer.close
debug = "".dup
io = ObservedMessageIO.new(reader, debug_output: debug)
begin
  io.each_message_chunk { raise "stop" }
rescue => error
  p error.class
end
p io.debug_output.equal?(debug)
io.each_message_chunk { |line| p line }
reader.close

# A nonlocal exit must restore all three pieces of write state too.
reader, writer = IO.pipe
debug = "".dup
io = ObservedMessageIO.new(writer, debug_output: debug)
p(io.write_message_by_block do |dest|
  dest.write("first\r\n")
  break
end)
p io.debug_output.equal?(debug), io.wbuf, io.written_bytes
p io.write_message("second\r\n")
writer.close
p reader.read
reader.close
