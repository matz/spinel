# class_exec and module_exec with arguments, and class_eval with a block
# parameter, in a class body: the block's parameters are the arguments
# (the class, for class_eval), also inside a define_method block of it,
# and an argument that reads a local of the body reads it. In an included
# hook, on the hook's parameter, each includer with its own values.
class CeA
  v = [1, 2]
  w = 10
  class_exec(v) { |list| define_method(:list) { list } }
  class_exec(v.size) { |n| p n }
  class_exec(v.map { |e| e + w }, w) do |m, k|
    p m, k
    define_method(:mk) { [m, k] }
  end
  class_exec(w) { |w2| define_method(:w2) { w2 + 1 } }
  p v, w
end
a = CeA.new
p a.list, a.mk, a.w2
module CeM
  module_exec(:k, "s") { |k, s| define_method(:key) { [k, s] } }
end
class CeB; include CeM; end
p CeB.new.key

class CeK
  class_eval do |k|
    p k
    define_method(:host) { k.name }
  end
  module_eval { |m| p m.name }
end
p CeK.new.host

module CeEvals
  def self.included(base)
    base.class_exec(7) { |n| define_method(:seven) { n } }
    base.class_exec(base.name, 2) do |nm, k|
      define_method(:tag) { "#{nm}#{k}" }
    end
  end
end
class CeH; include CeEvals; end
class CeHh; include CeEvals; end
p CeH.new.seven, CeH.new.tag, CeHh.new.tag

module CeYielding
  def self.included(base)
    base.class_eval do |k|
      p k
      define_method(:host) { k.name }
    end
  end
end
class CeY; include CeYielding; end
p CeY.new.host
