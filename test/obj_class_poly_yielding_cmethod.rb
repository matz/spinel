# obj.class.cmeth on a poly object reaches a yielding class method, with a
# block and without one, through the class value obj.class answers
module M
  class Base
    def self.pick(tag)
      block_given? ? yield(tag) : tag.to_s.size
    end
  end

  class Child < Base
  end
end

class Other
  def self.pick(tag) = 7
end

[M::Child.new, Other.new, M::Base.new].each { |o| p o.class.pick(:zz) }
[M::Child.new, Other.new].each { |o| p o.class.pick(:zz) { |t| [t] } }
