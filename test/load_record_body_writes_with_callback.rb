# A body that only writes an instance variable is not made a defining one by
# a method the runtime calls back by itself (to_s) that does define: an
# assignment calls nothing back. The class method's call in its class body
# is code the record still follows, so the method calling it after a later
# redefinition binds each version the way CRuby does.
class P
  def to_s = (Object.const_set(:ZZ, 1); "p")
end
class J; def self.v = (@x = 1); v; end
def fj = J.v
puts fj
def J.v = 2
puts fj
