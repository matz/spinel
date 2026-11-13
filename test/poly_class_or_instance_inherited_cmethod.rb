# A receiver that may be a class or an instance, calling a class method the
# classes inherit: the classes whose arms call the same method share one arm,
# which hands the method the class the value is, so `self` (here `name`) is
# that class in each.
class Rel
  def initialize(m, n)
    @m = m
    @n = n
  end

  def zap = "#{@m.name} rel #{@n}"
  def scale(k) = @n * k
end

class Base
  def self.all = Rel.new(self, 7)
  def self.zap = "#{name} via #{all.zap}"
  def self.scale(k) = name.size * k
end
class A < Base; end
class B < Base; end
class C < Base
  def self.zap = "C's own"
end
class D < C; end
class E < B; end

module Plain; end

def pick(i)
  case i
  when 0 then A
  when 1 then B
  when 2 then C
  when 3 then D
  when 4 then E
  when 5 then Base
  when 6 then Plain
  when 7 then String
  when 8 then "a string"
  else Rel.new(A, i)
  end
end

(0..9).each do |i|
  v = pick(i)
  begin
    p [i, v.zap, v.scale(3)]
  rescue NoMethodError => e
    p [i, e.class]
  end
end
