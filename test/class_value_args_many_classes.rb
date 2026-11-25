# A class method called with arguments on a Class value that may be any of
# many classes (a run-time const_get): the arguments run once, in order,
# whatever they are -- a local, a literal, an ivar, a call, a keyword --
# and the class the value holds answers.
class Cv18; end
class Cv19; end
class Cv0; def self.m(a, b = 1, k: 0) = [:cv0, a, b, k]; end
class Cv1; def self.m(a, b = 1, k: 0) = [:cv1, a, b, k]; end
class Cv2; def self.m(a, b = 2, k: 9) = [:cv2, a * 2, b, k]; end
class Cv3; def self.m(*r, **kw) = [:cv3, r, kw]; end
class Cv4; def self.m(a) = [:cv4, a]; end
class Cv5; def self.m(a, b = 1, k: 0) = [:cv5, a, b, k]; end
class Cv6; def self.m(a, b = 1, k: 0) = [:cv6, a, b, k]; end
class Cv7; def self.m(a, b = 1, k: 0) = [:cv7, a, b, k]; end
class Cv8; def self.m(a, b = 1, k: 0) = [:cv8, a, b, k]; end
class Cv9; def self.m(a, b = 1, k: 0) = [:cv9, a, b, k]; end
class Cv10; def self.m(a, b = 1, k: 0) = [:cv10, a, b, k]; end
class Cv11; def self.m(a, b = 1, k: 0) = [:cv11, a, b, k]; end
class Cv12; def self.m(a, b = 1, k: 0) = [:cv12, a, b, k]; end
class Cv13; def self.m(a, b = 1, k: 0) = [:cv13, a, b, k]; end
class Cv14; def self.m(a, b = 1, k: 0) = [:cv14, a, b, k]; end
class Cv15; def self.m(a, b = 1, k: 0) = [:cv15, a, b, k]; end
class Cv16; def self.m(a, b = 1, k: 0) = [:cv16, a, b, k]; end
class Cv17; def self.m(a, b = 1, k: 0) = [:cv17, a, b, k]; end

$trace = []
def arg(x) = ($trace << x; x)

class Runner
  def initialize(names) = @names = names
  def go
    @v = 7
    out = []
    @names.each_with_index do |n, i|
      k = Object.const_get(n)
      out << k.m(i)
      out << k.m(i, 3)
      out << k.m(@v, arg(i), k: arg(-i))
      out << k.m(arg(10 + i))
    end
    out
  end
end

names = %w[Cv0 Cv1 Cv2 Cv5 Cv17]
Runner.new(names).go.each { |r| p r }
p $trace
names2 = %w[Cv3 Cv4]
names2.each do |n|
  k = Object.const_get(n)
  p k.m(1)
  begin
    p k.m(1, 2)
  rescue ArgumentError => e
    p e.message
  end
end
begin
  Object.const_get("Cv" + 18.to_s).m(1)
rescue NoMethodError => e
  p e.message
end
