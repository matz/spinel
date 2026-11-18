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
file.close
File.unlink(path)
