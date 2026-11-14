# spinel: gc-stress
s = +"abcdef"
alias_of_s = s
before = s.hash
escaped = nil
result = IO::Buffer.for(s) do |buffer|
  escaped = buffer
  p [buffer.external?, buffer.readonly?, buffer.size]
  buffer.clear(90, 1, 2)
  p alias_of_s
  p before != s.hash
  buffer.slice(3, 2).set_string("XY")
  p s
  begin
    buffer.clear(1, 5, 2)
  rescue ArgumentError => error
    puts error.message
  end
  begin
    alias_of_s << "extra"
  rescue RuntimeError => error
    puts error.message
  end
  begin
    s.setbyte(0, 0)
  rescue RuntimeError => error
    puts error.message
  end
  begin
    s.freeze
  rescue RuntimeError => error
    puts error.message
  end
  :answer
end
p [result, escaped.null?, escaped.size, s.frozen?]
s << "!"
p s

begin
  IO::Buffer.for(s) do |buffer|
    buffer.set_value(:U8, 0, 65)
    raise "from block"
  end
rescue RuntimeError => error
  puts error.message
end
s.setbyte(1, 66)
p s

frozen = "xyz".freeze
p IO::Buffer.for(frozen) { |buffer| [buffer.readonly?, buffer.get_string] }
begin
  IO::Buffer.for(frozen) { |buffer| buffer.clear(0) }
rescue IO::Buffer::AccessError => error
  puts error.message
end

empty = +""
IO::Buffer.for(empty) { |buffer| p [buffer.null?, buffer.size, buffer.get_string] }

def leave_buffer(string)
  IO::Buffer.for(string) do |buffer|
    p buffer.get_value(:U8, 1)
    return :returned
  end
end

binary = +"a\0c".b
answer = IO::Buffer.for(binary) do |buffer|
  buffer.slice(1, 2).set_value(:U8, 0, 90)
  break :broken
end
p [answer, binary.bytes]
p leave_buffer(binary)
binary << "!"
p binary.bytes
