# A Struct's or Data's `new` the program defines before every call of it
# runs from a method or a lambda too, wherever the def stands: at the top
# level, in the builder's block, in `class << C`, with keywords.
N1 = Struct.new(:a)
def N1.new(*a) = :n1
def mk1 = N1.new(1)
p mk1
N2 = Struct.new(:a) do
  def self.new(*a) = :n2
end
def mk2 = N2.new(1)
p mk2, N2.new(2)
N3 = Struct.new(:a)
class << N3
  def new(*a) = :n3
end
def mk3 = N3.new(1)
p mk3
N4 = Data.define(:v)
def N4.new(*a) = :n4
mk4 = -> { N4.new(1) }
p mk4.()
N5 = Struct.new(:a, keyword_init: true)
def N5.new(**k) = :n5
def mk5 = N5.new(a: 1)
p mk5
