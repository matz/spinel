class Base
  def self.pick(tag) = [name, tag]
  def pick = self.class.pick(:own)
end

class Child < Base
  def self.pick(tag)
    block_given? ? yield(tag) : [:child, tag]
  end
end

[Base.new, Child.new].each { |o| p o.pick }
p Child.pick(:b) { |t| [:block, t] }
