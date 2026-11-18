# StringIO uses the selected class display result with its recorded length.
require "stringio"
class Named
  def self.to_s = "hi\0tail"
end

class ByteString < String
end
class InheritedName
  def self.to_s(text = ByteString.new("hello\0world")) = text
end
class ChildName < InheritedName
end

$called = 0
class NoName
  def self.to_s
    $called += 1
    nil
  end
end

io = StringIO.new
io.print Named
p io.string.bytes
io = StringIO.new
io.print ChildName
p io.string.bytes
io = StringIO.new
io.print NoName
p [$called, io.string.start_with?("#<Class:0x"), io.string.end_with?(">")]
p [io.string == "#{NoName}", $called]
