# TSort included into a Hash subclass that names its nodes and children:
# tsort, tsort_each, the strongly connected components and the walk from one
# node, with and without a block, and TSort::Cyclic for a cycle.
require "tsort"

class Deps < Hash
  include TSort
  alias tsort_each_node each_key
  def tsort_each_child(node, &block)
    fetch(node).each(&block)
  end
end

d = Deps.new
d[1] = [2, 3]
d[2] = [3]
d[3] = []
d[4] = []
p d.tsort
p d.tsort_each.to_a
d.tsort_each { |n| print n, " " }
puts
p d.strongly_connected_components
p d.each_strongly_connected_component.to_a
d.each_strongly_connected_component { |c| p c }
p d.each_strongly_connected_component_from(2).to_a
d.each_strongly_connected_component_from(1) { |c| p c }

r = Deps.new
r[1] = [2]
r[2] = [3, 4]
r[3] = [2]
r[4] = []
p r.strongly_connected_components
begin
  r.tsort
rescue TSort::Cyclic => e
  p e.class, e.message
end
p TSort::Cyclic.ancestors.include?(StandardError)
p TSort::VERSION
