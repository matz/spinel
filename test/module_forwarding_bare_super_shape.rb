# A module method `def m(...)` whose bare `super` reaches the method of an
# including class forwards every channel the caller passed.

class Base
  def process(action, *rest, **opts)
    [:base, action, rest, opts]
  end

  def handle(action, ...)
    run(action, ...)
  end

  def run(a, *r, **k, &b)
    [:run, a, r, k, b ? b.call : nil]
  end

  def lead(a, b = 2, *r)
    [:lead, a, b, r]
  end
end

module Wrap
  def process(...)
    super
  end

  def handle(...)
    x = 1
    super
  ensure
    x = 2
  end

  def lead(a, ...)
    super
  end
end

class C < Base
  include Wrap

  def dispatch(name)
    handle(name)
  end
end

c = C.new
p c.process("index")
p c.process("create", 1, k: 2)
p c.dispatch("show")
p c.handle("edit", 3, 4, z: 5) { :blk }
p c.lead(1)
p c.lead(1, 9, 8)
