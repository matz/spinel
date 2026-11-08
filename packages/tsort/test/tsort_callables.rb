# TSort's module functions over two Method objects, one walking the nodes
# and one a node's children, with string nodes, and the NotImplementedError
# of an includer that defines neither method.
require "tsort"

class Graph
  def initialize(edges)
    @edges = edges
  end

  def each_node(&block)
    @edges.each_key(&block)
  end

  def each_child(node, &block)
    @edges.fetch(node).each(&block)
  end
end

g = Graph.new("app" => ["lib", "log"], "lib" => ["log"], "log" => [], "doc" => ["app"])
each_node = g.method(:each_node)
each_child = g.method(:each_child)
p TSort.tsort(each_node, each_child)
p TSort.tsort_each(each_node, each_child).to_a
p TSort.strongly_connected_components(each_node, each_child)
TSort.each_strongly_connected_component(each_node, each_child) { |c| p c }
TSort.each_strongly_connected_component_from("app", each_child) { |c| p c }

c = Graph.new("a" => ["b"], "b" => ["a"])
begin
  TSort.tsort(c.method(:each_node), c.method(:each_child))
rescue TSort::Cyclic => e
  puts e.message
end

class Empty
  include TSort
end

begin
  Empty.new.tsort
rescue NotImplementedError => e
  p e.class
end
