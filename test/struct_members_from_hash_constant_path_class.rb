# Struct.new and Data.define read their members from a literal Hash
# constant at a path when the path's parent names the class whose body
# writes it: by a nested header, a reopening, a constant that holds the
# class, or a module another module names.
module Outer
  class Lock
    DEFAULTS = { retries: 3 }.freeze
  end
  D = Struct.new(*Lock::DEFAULTS.keys)
  p D.members
end
C = Struct.new(*Outer::Lock::DEFAULTS.keys)
p C.members
class Outer::Gate
  OPTS = { r: 1 }.freeze
end
E = Struct.new(*Outer::Gate::OPTS.keys)
p E.members
class Bolt; end
class Bolt
  BOLT_KEYS = { s: 1, t: 2 }.freeze
end
B = Bolt
F = Data.define(*B::BOLT_KEYS.keys)
p F.members
module A
  module Pin
    KEYS = { x: 1 }.freeze
  end
end
module Z
  Pin = A::Pin
end
H = Struct.new(*Z::Pin::KEYS.keys)
p H.members
