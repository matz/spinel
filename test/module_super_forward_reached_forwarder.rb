# A module forwarder whose super reaches another module's `def zork(...) =
# super` that is lowered after it takes the channels of the defs of the name,
# not of the plain defs it reaches alone: Wrap is lowered first, Inner still
# takes `...`, and Plain's `zork(a)` is the other def it reaches. The keyword
# of `C1.new.zork(1, k: 5)` stays a keyword on the way to Root.

class Root
  def zork(a, b = 2, k: 3) = [:root, a, b, k]
end
module Wrap
  def zork(...) = super
end
module Inner
  def zork(...) = super
end
class Plain
  def zork(a) = [:plain, a]
end
class C1 < Root
  include Inner
  include Wrap
end
class C2 < Plain
  include Wrap
end
p C1.new.zork(1, k: 5)
p C1.new.zork(1, 9)
p C2.new.zork(1)
