# A name an alias makes that a builtin answers too, by running a block the
# program stored (Enumerator#count runs the Enumerator's block), still may
# define, whatever the old name (here an attr reader): `go(V)` runs V's
# block, which redefines Base.sfx, so the macro after it waits, refused.
module Macros
  def attribute(name) = define_method("#{name}_#{sfx}") { name.to_s }
end
class Item
  attr_reader :cnt
  alias count cnt
end
V = Enumerator.new { |y| Base.define_singleton_method(:sfx) { "b" }; y << 1 }
def go(x) = x.count
class Base
  extend Macros
  def self.sfx = "a"
  go(V)
  attribute :title
end
p Base.instance_methods(false).sort
