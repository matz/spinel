# A block whose value is a bare `super` into a parent that takes the
# caller's block answers the parent's value through the yield.
class Base
  def self.transaction
    result = yield
    result
  end
end
class Core < Base
  def fire(name, &block)
    yield if block_given?
    name == :go
  end
end
class M < Core
  def tx = yield
  def fire(name, &block)
    tx { super }
  end
  def fire_tx(name, &block)
    self.class.transaction { fire(name, &block) }
  end
end
p M.new.fire(:go)
p M.new.fire(:stop) { puts "blk" }
p M.new.fire_tx(:go) { puts "tx blk" }
