class Offset
  def initialize(text)
    @text = text
  end

  def to_int
    @text << ("x" * 256)
    0
  end
end

text = +"a"
text << "\0b"
operands = [text, 1]
reader, writer = IO.pipe
p writer.syswrite(operands[0])
arguments = [operands[0]]
p writer.syswrite(*arguments)
writer.close
p reader.read.bytes
reader.close

path = "spinel_io_write_#{Process.pid}.tmp"
file = File.open(path, "w+b")
handles = [file, 1]
p file.pwrite(operands[0], 0)
p handles[0].pwrite(operands[0], 0)

def offsets(n) = Array.new(n, 0)

[0, 1, 2].each do |count|
  begin
    p file.pwrite(operands[0], *offsets(count))
  rescue ArgumentError => error
    puts error.message
  end
end
args = [operands[0], 0]
p handles[0].pwrite(*args)
file.rewind
p file.read.bytes
offset = [Offset.new(text)]
p handles[0].pwrite(operands[0], *offset)
file.rewind
p [file.read.bytesize, text.bytesize]

class Rendered
  def initialize(text)
    @text = text
  end

  def to_s
    puts "render"
    @text
  end
end

text = +"a\0b"
rendered = [Rendered.new(text), 1]
p handles[0].pwrite(rendered[0], Offset.new(text))
file.rewind
bytes = file.read
p [bytes.bytesize, bytes.bytes[0, 3]]
file.truncate(0)
match = [/a.b/m.match("a\0b"), 1]
p handles[0].pwrite(match[0], 0)
file.rewind
p file.read(3).bytes

text = +"a\0b"
errors = [RuntimeError.new(text), 1]
p handles[0].pwrite(errors[0], Offset.new(text))
file.rewind
p [file.read.bytesize, text.bytesize]

def traced_arguments(value)
  puts "arguments"
  [value, 0]
end

p (if ENV["SPINEL_TEST_RECEIVER"]
     puts "other receiver"
     file
   else
     puts "receiver"
     file
   end).pwrite(*traced_arguments(match[0]))
file.close
File.unlink(path)
