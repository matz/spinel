class Base
  def self.pick(tag)
    block_given? ? yield(tag) : [name, tag]
  end

  def pick = self.class.pick(:own)
end

class Child < Base
end

[Base.new, Child.new].each { |o| p o.pick }
klass = ARGV.empty? ? Child : Base
p klass.pick(:var)
p klass.pick(:blk) { |t| [:block, t] }
