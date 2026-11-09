# method_defined? on a Struct or Data whose members come from a splat
# (`Struct.new(*MEMS)`) reads the names the list holds, as the class
# registration does. The guard was read as "not defined", so the program's
# own def replaced the member reader.
MEMS = [:mem, :other]
S1 = Struct.new(*MEMS)
unless S1.method_defined?(:mem, false)
  class S1; def mem = :replaced; end
end
p S1.new(5).mem
S2 = Struct.new(*MEMS.freeze)
p S2.method_defined?(:other, false), S2.method_defined?(:other=, false), S2.method_defined?(:nope, false)
REV = %i[x y].freeze
S3 = Struct.new(*REV.reverse)
unless S3.method_defined?(:y)
  class S3; def y = :replaced; end
end
p S3.new(1, 2).y
STRS = ["aa", "bb"]
S4 = Struct.new(*STRS.map(&:to_sym))
unless S4.method_defined?(:aa)
  class S4; def aa = :replaced; end
end
p S4.new(7).aa
D1 = Data.define(*MEMS)
p D1.method_defined?(:mem, false), D1.method_defined?(:mem=, false)
unless D1.method_defined?(:mem, false)
  D1.class_eval { def mem = :replaced }
end
p D1.new(mem: 3, other: 4).mem
class S5 < Struct.new(*MEMS); end
p S5.method_defined?(:other), S5.new(1, 2).other
S6 = Struct.new(:first, *MEMS, keyword_init: true)
unless S6.public_method_defined?(:mem)
  class S6; def mem = :replaced; end
end
p S6.new(first: 0, mem: 9).mem
p S6.method_defined?(:first=), S6.method_defined?(:zzz)
unless S1.method_defined?(:zzz)
  class S1; def zzz = :added; end
end
p S1.new(1).zzz
def mems = [:q, :r]
