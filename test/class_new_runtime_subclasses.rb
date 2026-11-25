# Class#subclasses lists each class a Class.new made at run time from a
# static parent, on the constant, on self in a class method and on a
# class value read back from a boxed slot.

class K
  def self.kids = subclasses
end
def mk = Class.new(K) { def v = 1 }
p K.subclasses.size
a = mk
b = mk
p K.subclasses.size, K.subclasses.include?(a), K.subclasses.include?(b)
p K.kids.size, K.kids.include?(a)
x = [K, 1].first
p x.subclasses.size, x.subclasses.include?(b)
p K.subclasses.map { |k| k.new.v }
3.times { mk }
p K.subclasses.size
