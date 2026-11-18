# The boxed argument of method_capture_boxed_arg.rb, with the Method handed
# on as a parameter, kept in a Struct and a constant, passed as a block and
# converted with to_proc.
Entry = Struct.new(:name, :handler)
def kind(value) = value.class.name
def apply(callable, value) = callable.call(value)
def relay(callable, value) = apply(callable, value)
p kind(1)
m = method(:kind)
p relay(m, 1.5), relay(m, "s"), relay(->(v) { v }, :sym)
e = Entry.new("k", m)
p e.handler.call([1, 2].first), e.handler.(nil)
HANDLERS = [m, ->(v) { v }].freeze
p HANDLERS.map { |h| h.call(2.5) }
p [1.5, "t", nil].map(&m)
pr = m.to_proc
p [3, 4.5].map { |v| pr.call(v) }
p kind(2)
