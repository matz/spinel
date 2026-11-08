# Spinel package: tsort
#
# TSort from ruby/tsort 0.2.0: topological sorting and strongly connected
# components (Tarjan's algorithm) for an includer that defines
# tsort_each_node and tsort_each_child, or for two callables.
#
# Ruby's license or the 2-clause BSDL, see COPYING and BSDL beside this file.


module TSort

  VERSION = "0.2.0"

  class Cyclic < StandardError
  end

  def tsort
    each_node = method(:tsort_each_node)
    each_child = method(:tsort_each_child)
    TSort.tsort(each_node, each_child)
  end

  def self.tsort(each_node, each_child)
    tsort_each(each_node, each_child).to_a
  end

  def tsort_each(&block)
    each_node = method(:tsort_each_node)
    each_child = method(:tsort_each_child)
    TSort.tsort_each(each_node, each_child, &block)
  end

  def self.tsort_each(each_node, each_child)
    return to_enum(__method__, each_node, each_child) unless block_given?

    each_strongly_connected_component(each_node, each_child) {|component|
      if component.size == 1
        yield component.first
      else
        raise Cyclic.new("topological sort failed: #{component.inspect}")
      end
    }
  end

  def strongly_connected_components
    each_node = method(:tsort_each_node)
    each_child = method(:tsort_each_child)
    TSort.strongly_connected_components(each_node, each_child)
  end

  def self.strongly_connected_components(each_node, each_child)
    each_strongly_connected_component(each_node, each_child).to_a
  end

  def each_strongly_connected_component(&block)
    each_node = method(:tsort_each_node)
    each_child = method(:tsort_each_child)
    TSort.each_strongly_connected_component(each_node, each_child, &block)
  end

  def self.each_strongly_connected_component(each_node, each_child)
    return to_enum(__method__, each_node, each_child) unless block_given?

    id_map = {}
    stack = []
    each_node.call {|node|
      unless id_map.include? node
        each_strongly_connected_component_from(node, each_child, id_map, stack) {|c|
          yield c
        }
      end
    }
    nil
  end

  def each_strongly_connected_component_from(node, id_map={}, stack=[], &block)
    TSort.each_strongly_connected_component_from(node, method(:tsort_each_child), id_map, stack, &block)
  end

  def self.each_strongly_connected_component_from(node, each_child, id_map={}, stack=[])
    return to_enum(__method__, node, each_child, id_map, stack) unless block_given?

    minimum_id = node_id = id_map[node] = id_map.size
    stack_length = stack.length
    stack << node

    each_child.call(node) {|child|
      if id_map.include? child
        child_id = id_map[child]
        minimum_id = child_id if child_id && child_id < minimum_id
      else
        sub_minimum_id =
          each_strongly_connected_component_from(child, each_child, id_map, stack) {|c|
            yield c
          }
        minimum_id = sub_minimum_id if sub_minimum_id < minimum_id
      end
    }

    if node_id == minimum_id
      component = stack.slice!(stack_length .. -1)
      component.each {|n| id_map[n] = nil}
      yield component
    end

    minimum_id
  end

  def tsort_each_node
    raise NotImplementedError.new
  end

  def tsort_each_child(node)
    raise NotImplementedError.new
  end
end
