module Machines
  def machine(*names, &blk)
    blk ? blk.call(names) : [name, names]
  end
end

class Class
  include Machines
end

class Base
  def self.pick(*args)
    block_given? ? yield(args) : [name, args]
  end
end

class Child < Base
end

condition = ->(object) { object.class.machine(:state).first }
p condition.call(Child.new)
p condition.call(1)
klass = ARGV.empty? ? Child : Base
p klass.machine(:a, :b)
p klass.machine(:a, :b) { |n| n.size }
p klass.pick(:var)
p klass.pick(:a, :b) { |a| a.size }
