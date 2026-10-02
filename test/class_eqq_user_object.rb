# A builtin class `===` a user object answers false (or true up its class chain) instead of raising
class Pt; end
class MyA < Pt; end   # (a subclass of Array is refused, #7075)
S = Struct.new(:a)
D = Data.define(:x)
class Sub < S; end

o = Pt.new
p Integer === o, String === o, Float === o, Array === o, Hash === o, Symbol === o
p Numeric === o, Range === o, Proc === o, Time === o, Struct === o, Data === o

r = case o when Integer then :i when String then :s else :other end
p r

p Array === MyA.new, Pt === MyA.new
p Struct === S.new(1), Struct === Sub.new(2), Integer === S.new(3), Data === S.new(4)
p Data === D.new(x: 1), Struct === D.new(x: 2)

n = rand(2) > 5 ? Pt.new : nil
p Integer === n, NilClass === n, Pt === n
m = rand(2) > 5 ? nil : Pt.new
p NilClass === m, Pt === m

def kind(x)
  case x
  when Integer then :int
  when String then :str
  when Struct then :struct
  when Pt then :pt
  else :other
  end
end
p kind(Pt.new), kind(Sub.new(5)), kind(D.new(x: 3))
p kind(1), kind("a")
[Pt.new, 5, "s", nil].each { |v| p(Integer === v) }
