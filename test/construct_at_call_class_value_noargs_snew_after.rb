class A
  def initialize = @t = :a
  attr_reader :t
end
class B
  def initialize = @t = :b
  attr_reader :t
end
ks = [A, B]
ks.each { |k| p k.new.t }
class B
  def self.new = :b_custom
end
ks.each { |k| r = k.new; p(r.is_a?(Symbol) ? r : r.t) }
