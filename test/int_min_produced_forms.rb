# spinel: int64 -- -2^63 is an Integer past 2^31; not run on a 32-bit target
# Every way to produce exactly -2**63 without an overflow: the result is a
# number in all three modes. (A Bignum operand is folded to the same word.)
n = ARGV.size
def t(label)
  print label, ": "
  p yield
rescue => e
  puts "#{e.class}: #{e.message}"
end
t("sub")         { -9223372036854775807 - (n + 1) }
t("add")         { -9223372036854775807 + (n - 1) }
t("mul")         { -4611686018427387904 * (n + 2) }
t("neg lit")     { -(2**62) * 2 }
t("mul lit")     { -(1 << 62) * 2 }
t("to_i")        { ("-9223372036854775808" + "0" * n).to_i }
t("Integer()")   { Integer("-9223372036854775808" + "0" * n) }
t("to_i(16)")    { ("-8000000000000000" + "0" * n).to_i(16) }
t("to_i(2)")     { ("-1" + "0" * (63 + n)).to_i(2) }
t("unpack q<")   { ("\x00\x00\x00\x00\x00\x00\x00\x80" * (n + 1)).unpack1("q<") }
t("unpack q>")   { ("\x80\x00\x00\x00\x00\x00\x00\x00" * (n + 1)).unpack1("q>") }
t("Float#to_i")  { (-9223372036854775808.0 - n).to_i }
t("Float#floor") { (-9223372036854775808.0 - n).floor }
t("Float#ceil")  { (-9223372036854775808.0 - n).ceil }
t("Float#round") { (-9223372036854775808.0 - n).round }
t("Float#truncate") { (-9223372036854775808.0 - n).truncate }
t("Integer(f)")  { Integer(-9223372036854775808.0 - n) }
t("buffer s64")  { b = IO::Buffer.new(8); b.set_value(:s64, 0, -9223372036854775807 - (n + 1)); b.get_value(:s64, 0) }
t("buffer u64")  { b = IO::Buffer.new(8); b.set_value(:u64, 0, 0x8000000000000000 + n); b.get_value(:s64, 0) }
t("array min")   { [n, -9223372036854775807 - (n + 1)].min }
t("hash key")    { h = {}; h[-9223372036854775807 - (n + 1)] = 1; h.keys }
t("sum")         { [-9223372036854775807, -(n + 1)].sum }
t("divmod")      { (-9223372036854775807 - (n + 1)).divmod(1).first }
t("bit or")      { (1 << 62 | 1 << 61) | ~0x7fffffffffffffff }
t("rational")    { Rational(-9223372036854775807 - (n + 1), 1).to_i }
t("string fmt")  { ("%d" % (-9223372036854775807 - (n + 1))).to_i }
t("succ pred")   { (-9223372036854775807 - n).pred }
t("times")       { x = 0; 1.times { x = -9223372036854775807 - (n + 1) }; x }
t("inject")      { [-9223372036854775807, -(n + 1)].inject(:+) }
t("reduce")      { [-9223372036854775807, -(n + 1)].reduce(0) { |s, e| s + e } }
