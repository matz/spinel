# Literal-symbol get_value reads inline for integer types up to 32 bits and
# floats when the offset fits the owned bytes or a valid slice. Invalid
# views and offsets fall back to the checked read with its CRuby errors.
require "tmpdir"

def get(b, t, o)
  case t
  when :U8  then b.get_value(:U8, o)
  when :S8  then b.get_value(:S8, o)
  when :u16 then b.get_value(:u16, o)
  when :s16 then b.get_value(:s16, o)
  when :U16 then b.get_value(:U16, o)
  when :S16 then b.get_value(:S16, o)
  when :u32 then b.get_value(:u32, o)
  when :s32 then b.get_value(:s32, o)
  when :U32 then b.get_value(:U32, o)
  when :S32 then b.get_value(:S32, o)
  when :f32 then b.get_value(:f32, o)
  when :f64 then b.get_value(:f64, o)
  when :F32 then b.get_value(:F32, o)
  when :F64 then b.get_value(:F64, o)
  end
end

def probe(t, label, b, offs)
  offs.each do |o|
    begin
      puts "#{t} #{label} #{o}: #{get(b, t, o)}"
    rescue => e
      puts "#{t} #{label} #{o}: #{e.class}: #{e.message}"
    end
  end
end

def fill(b)
  i = 0
  while i < b.size
    b.set_value(:U8, i, (i * 73 + 129) % 256)
    i += 1
  end
  b
end

path = File.join(Dir.tmpdir, "sp_iob_inline_#{Process.pid}.bin")
File.binwrite(path, (0...16).map { |i| ((i * 73 + 129) % 256).chr }.join)

[:U8, :S8, :u16, :s16, :U16, :S16, :u32, :s32, :U32, :S32, :f32, :f64, :F32, :F64].each do |t|
  w = IO::Buffer.size_of(t)
  b = fill(IO::Buffer.new(16))
  probe(t, "owned", b, [0, 1, 16 - w, 17 - w, 16, -1, 2**30, -(2**30)])
  probe(t, "slice", b.slice(3, 9), [0, 9 - w, 10 - w, -1])
  probe(t, "zero", IO::Buffer.new(0), [0])
  probe(t, "short", fill(IO::Buffer.new(w - 1)), [0])
  f = fill(IO::Buffer.new(16))
  fs = f.slice(0, 8)
  f.free
  probe(t, "freed", f, [0])
  probe(t, "slice-of-freed", fs, [0])
  tr = fill(IO::Buffer.new(16))
  u = tr.transfer
  probe(t, "transferred-from", tr, [0])
  probe(t, "transferred-to", u, [16 - w])
  r = fill(IO::Buffer.new(16))
  rs = r.slice(8, 8)
  r.resize(4)
  probe(t, "slice-of-shrunk", rs, [0])
  probe(t, "shrunk", r, [0, 4 - w, 5 - w])
  File.open(path) do |io|
    probe(t, "mapped", IO::Buffer.map(io, nil, 0, IO::Buffer::READONLY), [0, 16 - w, 17 - w])
  end
  probe(t, "for", IO::Buffer.for("ABCDEFGHIJ"), [0, 10 - w])
end
File.delete(path)
