# The boxed argument of method_capture_boxed_arg.rb, with the Method handed
# through two adapters before the lambda that calls it.
def scale(value)
  value * 2
end
def inner(callback)
  ->(value) { callback.call(value) }
end
def outer(options)
  inner(options.fetch(:loader))
end
p scale(1)
f = outer({loader: method(:scale)})
g = outer({loader: proc { |v| v }})
p f.call(1.5)
p g.call("s")
p f.call("ab")
p f.call(3)
