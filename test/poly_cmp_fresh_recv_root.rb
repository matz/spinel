# spinel: gc-stress
# A boxed comparison holds its receiver while the argument allocates. The
# member read out of a fresh Data, Struct or object, a value read out of a
# fresh Hash, a fresh `x + y` and a fresh interpolation were held only in a C
# argument while an interpolation built the other operand, and under
# SPINEL_GC_STRESS=2 the comparison read the freed value.
P = Data.define(:a, :b)
S = Struct.new(:a, :b)
class C
  attr_reader :a, :b
  def initialize(a, b) = (@a = a; @b = b)
end
def hb(i) = { b: "h#{i}" }
n = Hash.new(0)
40.times do |i|
  n[:data_eq] += 1 if P.new(a: "q", b: hb(i)[:b]).b == "h#{i}"
  n[:data_ne] += 1 unless P.new(a: "q", b: hb(i)[:b]).b != "h#{i}"
  n[:data_cmp] += 1 if (P.new(a: "q", b: hb(i)[:b]).b <=> "h#{i}") == 0
  n[:data_lt] += 1 if P.new(a: "q", b: hb(i)[:b]).b < "z#{i}"
  n[:data_eql] += 1 if P.new(a: "q", b: hb(i)[:b]).b.eql?("h#{i}")
  flag = i.even?
  n[:data_case_eq] += 1 if P.new(a: "q", b: hb(i)[:b]).b === (flag ? "h#{i}" : i)
  n[:str_case_eq] += 1 if "h#{i}" === (flag ? "h#{i}" : i)
  n[:struct_eq] += 1 if S.new("q", hb(i)[:b]).b == "h#{i}"
  n[:object_eq] += 1 if C.new("q", hb(i)[:b]).b == "h#{i}"
  n[:hash_eq] += 1 if hb(i)[:b] == "h#{i}"
  n[:hash_cmp] += 1 if (hb(i)[:b] <=> "h#{i}") == 0
  n[:sum_eq] += 1 if hb(i)[:b] + "x" == "h#{i}x"
  n[:and] += 1 if i >= 0 && hb(i)[:b] == "h#{i}"
  n[:or] += 1 if i < 0 || hb(i)[:b] == "h#{i}"
  n[:ternary] += 1 if (i.odd? ? hb(i)[:b] != "h#{i}" : hb(i)[:b] == "h#{i}") == i.even?
end
p n
