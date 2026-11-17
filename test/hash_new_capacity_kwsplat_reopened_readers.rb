# Hash.new's `capacity:` read out of a `**h` binds the keyword as CRuby
# does, without calling the program's own Hash#keys, Hash#fetch,
# Hash#is_a?, Array#- or Array#empty?: only a non-Hash operand's to_hash
# runs. Through a prepended forward too.
class Hash
  def keys = raise("own keys")
  def fetch(*) = raise("own fetch")
  def is_a?(x) = raise("own is_a?")
end
class Array
  def -(o) = raise("own minus")
  def empty? = raise("own empty?")
end
class Conf
  def to_hash = { capacity: 5 }
end
h = { capacity: 4 }
p((Hash.new(9, **h)[:x] rescue $!.message))
p((Hash.new(**h)[:x] rescue $!.message))
p((Hash.new(9, **Conf.new)[:x] rescue $!.message))
g = { capacity: 4, foo: 1 }
p((Hash.new(9, **g)[:x] rescue $!.message))
module Sized
  def initialize(d = 3, capacity: 1) = super
end
Hash.prepend(Sized)
p((Hash.new(**h)[:x] rescue $!.message))
p((Hash.new(7, **h)[:x] rescue $!.message))
p((Hash.new(foo: 1) rescue $!.message))
