# Struct.new(*Outer::Lock::DEFAULTS.keys) where the one write of DEFAULTS is
# in a top-level class Lock, not in Outer::Lock. CRuby raises NameError
# (Outer::Lock has no DEFAULTS). The path names another class than the one
# the write's body opens, though both have the last name Lock, so the
# members are not read from that Hash: it is refused.
class Lock
  DEFAULTS = { retries: 3, timeout: 1.5 }.freeze
end
module Outer
  class Lock
  end
end
C = Struct.new(*Outer::Lock::DEFAULTS.keys)
p C.members
