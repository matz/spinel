# Only a subclass's instance method does `def self.m`. A call on a value
# typed as the superclass holds only superclass instances, so it compiles
# and runs the instance method, as CRuby does.
class B
  def m = "inst"
end
class S < B
  def setup
    def self.m = "single"
  end
end
puts B.new.m
[B.new, B.new].each { |x| puts x.m }
s = S.new
s.setup
