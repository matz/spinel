# A singleton_class.attr_* call whose value an assignment keeps: CRuby's
# attr_* answers the names it defined ([:a, :a=] for attr_accessor :a). The
# call is a declaration on the class, as its statement form is, and the
# assignment takes that Array. Before, only the statement form was read, and
# `x = singleton_class.attr_accessor :a` was refused as Object#singleton_class
# (a configuration module keeping the names it declared, to delegate them
# later).
module Negotiation
  names = @names = singleton_class.attr_accessor :ignore_header, :strict_header
  p names, @names
  r = singleton_class.attr_reader :mode
  w = singleton_class.attr_writer :level
  p r, w
  @mode = :fast
end

class Store
  KEYS = singleton_class.attr_accessor :root
  p KEYS
end

extra = Store.singleton_class.attr_accessor :backup
p extra

Negotiation.ignore_header = true
Negotiation.level = 3
Store.root = "/srv"
Store.backup = "/bak"
p Negotiation.ignore_header, Negotiation.strict_header, Negotiation.mode
p Store.root, Store.backup
