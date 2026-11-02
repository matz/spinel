# Every mixin fires its module's hook: prepend fires prepended (one defined
# after a prepend fires only for the later ones), a module including a module
# fires included with that module, K.include(M) at the top level fires it
# with K, and a hook the module gets through extend fires once it has it.
module Pre
  def self.prepended(b) = puts("pre #{b}")
end
class PreA
  prepend Pre
end
module PreLate; end
class PreB; prepend PreLate; end
module PreLate
  def self.prepended(b) = puts("pre late #{b}")
end
class PreC; prepend PreLate; end

module Inner
  def self.included(b) = puts("inner #{b}")
end
module Outer
  include Inner
end
class Both; include Outer; end
p Both.ancestors.take(3)

module Top
  def self.included(b) = puts("top #{b}")
end
class TopK; end
TopK.include(Top)
p TopK.include?(Top)

module Hooks
  def included(b) = puts("hooked #{b}")
end
module Plain; end
class PlainA; include Plain; end
Plain.extend(Hooks)
class PlainB; include Plain; end
puts "done"
