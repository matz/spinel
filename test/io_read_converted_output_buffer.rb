class BufferCarrier
  def initialize(string)
    @string = string
  end

  def to_str
    puts :convert
    @string
  end
end

original = +"old"
carrier = BufferCarrier.new(original)
r, w = IO.pipe
w.write("abcdefgh")
w.close
p [r.read(2, carrier).equal?(original), original]
p [r.readpartial(2, carrier).equal?(original), original]
p [r.sysread(2, carrier).equal?(original), original]
p [r.read_nonblock(2, carrier).equal?(original), original]
p [r.read(2, carrier), original]
r.close

r, w = IO.pipe
w.write("abc")
w.close
s = +"old"
p [r.read(0, s).equal?(s), s]
s << "grow"
p [r.readpartial(0, s).equal?(s), s]
s << "grow"
p [r.read_nonblock(0, s).equal?(s), s]
s << "grow"
p [r.sysread(0, s).equal?(s), s]
p r.read(3)
r.close

closed_buffer = +"unchanged"
p [r.sysread(0, closed_buffer).equal?(closed_buffer), closed_buffer]

def read_to_end(binary, buffer)
  r, w = IO.pipe
  r.binmode if binary
  w.write("é")
  w.close
  p [r.read(nil, buffer).equal?(buffer), buffer.bytes, buffer.encoding.name]
  p [r.read(nil, buffer).equal?(buffer), buffer.bytes, buffer.encoding.name]
  r.close
end

read_to_end(false, "seed".b)
read_to_end(true, +"seed")

def read_gathered(file, *arguments)
  file.read(*arguments)
end

r, w = IO.pipe
w.write("abc")
w.close
buffer = +"old"
p [read_gathered(r, 2, buffer).equal?(buffer), buffer]
p [read_gathered(r, 2, buffer).equal?(buffer), buffer]
p [read_gathered(r, 2, buffer), buffer]
r.close
