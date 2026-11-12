# CRuby's pp lays a Struct or a Data out by its own pretty_print, so the
# class's own #inspect does not run for it, in an Array, a Hash or another
# record either; p and inspect still run it
S = Struct.new(:a) do
  def inspect = "S<#{a}>"
end
D = Data.define(:v) do
  def inspect = "D(#{v})"
end
class P < Struct.new(:x, :y)
  def inspect = "P!"
  def to_s = "Pts"
end
Q = Struct.new(:inner, :n)
E = Data.define(:a, :b)
pp S.new(1)
pp [S.new(2)]
pp D.new(1)
pp [D.new(2)]
pp P.new(1, 2)
pp Q.new(S.new(5), 6)
pp E.new(a: D.new(3), b: [S.new(4)])
pp({k: S.new(8), d: D.new(9)})
pp S.new(1), D.new(2)
p S.new(3), D.new(3), P.new(1, 2)
p Q.new(S.new(5), 6)
puts S.new(10).inspect
x = pp(D.new(12), S.new(13))
p x
p pp(S.new(14)).a
# a class's own #inspect that pp calls (not a record's) runs as outside
# a pp: the records it inspects show their own #inspect
class W
  def initialize(s) = @s = s
  def inspect = "W(#{@s.inspect})"
end
pp W.new(S.new(7))
pp [W.new(D.new(8))]
