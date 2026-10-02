class Base
  def self.pick(a, b)
    block_given? ? yield(a) : [name, a, b]
  end
end

class Child < Base
end

args = [:x, :y]
klass = ARGV.empty? ? Child : Base
p klass.pick(*args)
p klass.pick(*args) { |v| [:block, v] }
p klass.pick(:p, :q)
