require "tmpdir"

class PreadBuffer
  attr_reader :value, :calls

  def initialize
    @value = +"seed"
    @calls = 0
  end

  def to_str
    @calls += 1
    @value
  end
end

path = File.join(Dir.tmpdir, "spinel-pread-buffers-#{Process.pid}")
File.binwrite(path, "A\0B")
begin
  File.open(path) do |file|
    typed_buffer = +"seed"
    typed = file.pread(0, 1, typed_buffer)
    p [typed.equal?(typed_buffer), typed, typed.encoding.name]
    typed = file.pread(2, 1, typed_buffer)
    typed << "t" * 90
    p [typed_buffer.bytesize, typed_buffer.getbyte(0), typed_buffer.encoding.name]
    begin
      file.pread(4, 10, typed_buffer)
    rescue EOFError
      p [typed_buffer.bytes, typed_buffer.encoding.name]
    end

    io = [file, 1][ARGV.size]
    stored = [+"seed", 1]
    buffer = stored[0]
    answer = io.pread(0, 1, buffer)
    p [answer.equal?(buffer), stored[0].bytes]
    answer = io.pread(3, 0, buffer)
    answer << "b" * 90
    p [stored[0].bytesize, stored[0].getbyte(1), buffer.encoding.name]
    begin
      io.pread(1, 10, buffer)
    rescue EOFError
      p [stored[0].bytes, buffer.encoding.name]
    end
    p [io.pread(0, 0, nil).bytes, io.pread(1, 0, nil).encoding.name]

    wrapper = PreadBuffer.new
    converted = io.pread(3, 0, wrapper)
    p [converted.equal?(wrapper.value), converted.bytes, wrapper.calls]
    converted << "g" * 64
    p wrapper.value.bytesize
    begin
      io.pread(1, 10, wrapper)
    rescue EOFError
      p [converted.equal?(wrapper.value), converted.bytes, wrapper.calls]
    end
  end
ensure
  File.delete(path)
end
