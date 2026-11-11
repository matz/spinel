# A yielding method the program defines on NilClass or String answers its
# block's value, nil included: the call's Integer keeps its nil.
class NilClass
  def f; yield 1; yield nil; end
  def g(a); yield a; end
  def h; yield 1; yield; end
end
p nil.f { |v| v }
p nil.g(nil) { |v| v }
p nil.g(4) { |v| v }
p nil.h { |v| v }
nil.h { |v| p v }
class String
  def s; yield 1; yield; end
end
"a".s { |v| p v }
p "a".s { |v| v }
