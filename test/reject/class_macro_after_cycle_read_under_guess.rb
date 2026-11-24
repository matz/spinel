# A class-body macro after a call that may define, found only through a
# cycle the record answers by a guess: `n` reads `m`'s answer while `r`'s
# cycle is still guessed, so `n`'s answer rests on that guess too. When the
# guess does not hold (`r` defines), `n` is asked again, and the macro after
# `n 9` waits for the definition it may make, as before: refused.
module Macros
  def attribute(name) = define_method("#{name}_#{sfx}") { name.to_s }
end
module H
  def r(k) = (m(k); n(k); k > 5 ? (def self.sfx = "b") : nil)
  def m(k) = k > 0 ? r(k - 1) : nil
  def n(k) = m(k)
end
class B
  extend Macros
  extend H
  def self.sfx = "a"
  r 0
  n 9
  attribute :title
end
p B.instance_methods(false).sort
