# A bare `instance_exec(&block)` runs the block on self. In a block a call
# hands to a yielding method that a subclass has its own copy of (the module
# is included again), the block is lifted into a proc for the class-id
# dispatch, and that proc did not capture self: the C named a `self` the
# proc function does not have. instance_eval is the same.
module Collect
  def with_list(list, &block)
    collecting(list) { instance_exec(&block) }
  end

  def with_list_eval(list, &block)
    collecting(list) { instance_eval(&block) }
  end

  def collecting(list)
    @list = list
    yield
    list
  end

  def add(s) = @list << s
end

class Base
  include Collect

  def initialize(name) = @name = name
  def build = with_list([]) { add("base #{@name}") }
  def build_eval = with_list_eval([]) { add("eval #{@name}") }
end

class Child < Base
  include Collect

  def build = with_list(["child"]) { add("child #{@name}") }
end

p Base.new("a").build
p Child.new("b").build
p Base.new("c").build_eval
p Child.new("d").build_eval
