# `K.instance_eval { def m }` defines K's class method: the load record's
# frame places the defs (nothing rewrites the block), on a constant receiver,
# on self in a class body, and through a module.
class K; end
K.instance_eval { def km = :km }
p K.km
class M
  self.instance_eval do
    def mm = "mm"
  end
  def self.other = mm + "!"
end
p M.mm, M.other
module Mod; end
Mod.instance_eval { def modm(x) = x * 2 }
p Mod.modm(21)
p K.respond_to?(:km), K.new.respond_to?(:km)
