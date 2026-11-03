# A module is no class (#10751): Class's own methods raise NoMethodError on
# it, and a class is no module: include, extend and prepend of one raise
# TypeError where they run, and mix nothing in, not even the modules named
# beside it.
module M; end
module Outer; module Inner; end; end
class K; end
begin; M.superclass; rescue NoMethodError => e; p [e.message, e.receiver]; end
begin; M.allocate; rescue NoMethodError => e; p [e.message, e.receiver]; end
begin; Outer::Inner.superclass; rescue NoMethodError => e; p e.message; end
p K.superclass, K.allocate.class
begin; Comparable.superclass; rescue NoMethodError => e; p e.message; end

module Mixed
  def mixed = :mixed
end
class L
  def before = :before
  begin
    include K, Mixed
  rescue TypeError => e
    puts "include: #{e.message}"
  end
  def after = :after
end
p L.new.before, L.new.after
p L.include?(Mixed), L.new.respond_to?(:mixed)
begin; class L2; extend K; end; rescue TypeError => e; puts "extend: #{e.message}"; end
begin; class L3; prepend K; end; rescue TypeError => e; puts "prepend: #{e.message}"; end
begin; class L4; include String; end; rescue TypeError => e; puts "builtin: #{e.message}"; end
class L5; include Comparable; include Mixed; end
p L5.include?(Comparable), L5.new.mixed

# what a class value answers about itself is the same whether the value is
# a constant or held in a local, and whether it is a class or a module
x = M
y = [M, K].sample
p M.class, K.class, x.class
p M.is_a?(Class), M.is_a?(Module), K.is_a?(Class), x.is_a?(Class)
p M.instance_of?(Module), K.instance_of?(Class), K.instance_of?(Module)
p Class === M, Module === M, Class === K
p M.inspect, M.to_s, x.name
p((x.superclass rescue :nme))
p((x.allocate rescue :nme))
p((x.new rescue :nme))
p((y.superclass rescue :nme).class)
p M.ancestors, K.ancestors, M.singleton_class.class
