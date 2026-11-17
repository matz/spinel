# A prepended initialize that forwards what it received stays the builtin's
# own where the call fills every positional parameter (its defaults never
# run), and with a keyword rest a bare `super` passes on.
module Fill
  def initialize(n = 3) = super(n)
end
Array.prepend(Fill)
p Array.new(2)
module Rest
  def initialize(*a, **kw) = super
end
String.prepend(Rest)
p String.new("ab")
# ...String's own `new` given keywords applies them itself and runs no initialize
p String.new("ab", encoding: "BINARY").encoding
