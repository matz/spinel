# `def self.m` inside an instance method, where the class also has an
# instance method m: the instance that ran the def runs its own m, which a
# compiled class cannot hold. The call is refused, instead of compiling to
# the instance method (CRuby prints "single").
class K
  def m = "inst"

  def setup
    def self.m = "single"
  end
end
k = K.new
k.setup
puts k.m
