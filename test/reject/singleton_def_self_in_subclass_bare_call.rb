# A subclass's instance method does `def self.m`, and a superclass method
# calls a bare m. Self there can be the subclass instance that ran the def,
# so the call runs that instance's own m (CRuby prints "single"). The call
# is refused, instead of compiling to a NameError.
class B
  def show
    m
  end
end
class S < B
  def setup
    def self.m = "single"
  end
end
s = S.new
s.setup
puts s.show
