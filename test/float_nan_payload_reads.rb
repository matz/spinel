# A NaN whose payload is 0x7FF8000000000001, read from bytes, is a Float in a
# plain Float local: printed as NaN, nan?, not nil, truthy, and arithmetic
# gives NaN. (The payload used to be the nil of a nullable Float slot.)
n = ARGV.size
x = ("\x01\x00\x00\x00\x00\x00\xF8\x7F" * (n + 1)).unpack1("d")
p x
puts x
puts x.to_s
puts x.inspect
puts "#{x}"
p x.nil?
p x.nan?
p(x ? "truthy" : "falsy")
p !x
p x == x
p x != x
p x.eql?(x)
p x.equal?(nil)
p x == nil
p x + 1.0
p x - 1.0
p x * 2.0
p x / 2.0
p -x
p (x + 1.0).nan?
p (x * 0).nan?
p x.abs.nan?
p x.finite?
p x.infinite?
p x.class
p(x || 0.0)
p [x].pack("E").unpack1("Q<").to_s(16)
p [x].pack("G").unpack1("Q>").to_s(16)
p [x].pack("d").bytes.size
p "%f" % x
p "%.2f" % x
p x.to_f.nan?
p [x].inspect
p [x].first.nan?
p x.zero?
p x.positive?
p x.negative?
p x <=> 1.0
p x < 1.0
p x > 1.0
y = [0x7FF8000000000001 + n].pack("Q<").unpack1("d")
p y.nan?
p y.nil?
puts "#{y}"
z = IO::Buffer.new(8)
z.set_value(:u64, 0, 0x7FF8000000000001 + n)
w = z.get_value(:f64, 0)
p w
p w.nan?
p w.nil?
