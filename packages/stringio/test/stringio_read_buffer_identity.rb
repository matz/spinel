require "stringio"

class FreezeReadBuffer
  def initialize(buffer)
    @buffer = buffer
  end

  def to_int
    @buffer.freeze
    GC.start
    2
  end
end

backing = +"abcd"
reader = StringIO.new(backing)
p reader.string.equal?(backing)
reader.pos = 1
value = reader.read(2, backing)
p [value.equal?(backing), backing.bytes, reader.pos]

replacement = +"abcd"
reader.string = replacement
reader.pos = 1
p reader.string.equal?(replacement)
p [reader.read(2, replacement).equal?(replacement), replacement.bytes, reader.pos]

buffer = +"before"
reader = StringIO.new("abc")
begin
  reader.read(FreezeReadBuffer.new(buffer), buffer)
rescue FrozenError
  p [buffer, reader.pos]
end

reader = StringIO.new("a")
buffer = +"before"
p reader.read(0, buffer).equal?(buffer)
reader.read
buffer << "before"
value = reader.read(0, buffer)
p [value.equal?(buffer), value.encoding.name, buffer, buffer.encoding.name]

buffer = +"before".b
value = StringIO.new("é").read(nil, buffer)
p [value.equal?(buffer), buffer.encoding.name, buffer.length, buffer.bytesize]
value = StringIO.new("é".b).read(nil, buffer)
p [value.equal?(buffer), buffer.encoding.name, buffer.length, buffer.bytesize]
