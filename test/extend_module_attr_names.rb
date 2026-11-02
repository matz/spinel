# Which modules' attr_accessor / attr_reader / attr_writer become class-side
# accessors: the modules a bare `extend` names as a constant or a constant
# path, however many arguments it has. A module that is only included keeps its
# instance accessors, and one nothing names has none on the module.
module ExtA
  attr_accessor :a
end

module ExtB
  attr_reader :b
  attr_writer :b2
  def b_pair = [b, @b2]
end

module Outer
  module ExtC
    attr_accessor :c
  end
end

module OnlyIncluded
  attr_accessor :inc
end

module Unused
  attr_accessor :unused
end

class Host
  extend ExtA, ExtB
  extend Outer::ExtC
  include OnlyIncluded
end

Host.a = 1
Host.b2 = 3
Host.instance_variable_set(:@b, 2)
Host.c = 4
p [Host.a, Host.b, Host.b_pair, Host.c]

h = Host.new
h.inc = :inc
p h.inc

p Unused.respond_to?(:unused)
p ExtA.respond_to?(:a)
