# spinel: int64
begin
  IO::Buffer.new((1 << 62) - 1, IO::Buffer::INTERNAL)
rescue IO::Buffer::AllocationError => error
  p error.class
end

p IO::Buffer.new(16).size
