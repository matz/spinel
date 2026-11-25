# A `...` forwarder no call names, of a name the runtime calls itself,
# takes what the runtime passes: one line per name of fwd_uncalled_rest
# (to_json, == and each have tests of their own).
class S
  def initialize = @s = "s"
  def to_s(...) = @s.to_s(...)
  def inspect(...) = @s.inspect(...)
end

class C
  include Comparable
  def initialize(n) = @n = n
  def <=>(...) = @n.<=>(...)
end

class H
  def initialize(n) = @n = n
  def hash(...) = @n.hash(...)
  def eql?(...) = true.eql?(...)
end

class A
  def initialize = @a = [1, 2]
  def to_a(...) = @a.to_a(...)
end

class K
  def initialize = @h = {k: 1}
  def to_hash(...) = @h.to_hash(...)
end

def kw(**o) = o

puts S.new                                   # to_s
p S.new                                      # inspect
p C.new(2) > 1, C.new(2).between?(1, 3)     # <=>
p({H.new(1) => :a}.size)                     # hash
p({H.new(1) => :a}.key?(H.new(1)))           # eql?
p [*A.new]                                   # to_a
p kw(**K.new)                                # to_hash
