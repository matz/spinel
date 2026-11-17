# A name built by joining a nested array (which CRuby flattens) is not
# computed at compile time: the macro is left as written (a run-time
# public_send, refused), not expanded with the nested part dropped.
module Consts
  def constant(c)
    const_set(c, public_send([["calc", "box"], c.to_s.downcase].join))
  end
end
class Calc
  extend Consts
  def self.calcboxsize = 1
  def self.size = 2
  constant :SIZE
end
p Calc::SIZE
