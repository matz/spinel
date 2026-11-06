# A Struct's `new` the program defines, called in a method only an entry
# reaches: the host calls it after the load, so the call meets the def the
# load made, not one in a method nothing calls. The library used to be
# refused, the call read as one in a body that never runs.
S = Struct.new(:a)
class S
  def self.new(x) = allocate.tap { |o| o.a = x + 1 }
end
module ExtLateNew
  def self.h(x) = S.new(x).a
  #: (Integer) -> Integer
  def self.e(x) = h(x)
  def self.d
    def S.new(x) = allocate.tap { |o| o.a = x * 10 }
  end
end
