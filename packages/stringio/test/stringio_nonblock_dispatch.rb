require "stringio"

class CustomStream < StringIO
  def read_nonblock(length, buffer = nil, exception: true)
    text = "custom:#{length}:#{exception}"
    return text unless buffer
    buffer.replace(text)
    buffer
  end
end

class ParentStream
  def read_nonblock(length, buffer = nil, exception: true)
    buffer ? buffer.replace("parent") : "parent"
  end
end
class ChildStream < ParentStream
end

class StreamHolder
  def initialize(stream)
    @stream = stream
  end
  def read
    self.stream.read_nonblock(1, +"", exception: false)
  end
  attr_reader :stream
end

p StreamHolder.new(CustomStream.new("ignored")).read
p StreamHolder.new(ChildStream.new).read
custom = { "stream" => CustomStream.new("ignored") }["stream"]
p custom.read_nonblock(2, +"", exception: false)
p custom.send(:read_nonblock, 3, +"", exception: false)

def returned_stream(stream)
  stream
end
p returned_stream(custom).read_nonblock(4, +"", exception: false)

reader, writer = IO.pipe
writer.write("pipe")
writer.close
streams = { "memory" => StringIO.new("text"), "pipe" => reader }
buffer = +"old"
p streams["memory"].read_nonblock(4, buffer, exception: false), buffer
p streams["pipe"].read_nonblock(4, buffer, exception: false), buffer
p streams["memory"].read_nonblock(1, buffer, exception: false), buffer
p streams["pipe"].read_nonblock(1, buffer, exception: false), buffer
reader.close
