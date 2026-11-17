# A bare `super` into a parent that calls its &block with `&.` forwards the
# caller's block, nil when the caller got none: the call is skipped then.
class Core
  def fire(name, &block)
    got = block&.call(name)
    [name, got]
  end
end
class M < Core
  def fire(name, &block)
    super
  end
end
class N < Core
  def fire(name, &blk)
    super(name, &blk)
  end
end
p M.new.fire(:go)
p M.new.fire(:stop) { |n| "blk #{n}" }
p N.new.fire(:go)
p N.new.fire(:stop) { |n| n.to_s.upcase }
class E < Core
  def fire(name, &block)
    [1].each { return super }
  end
end
p E.new.fire(:go)
p E.new.fire(:stop) { |n| n }
