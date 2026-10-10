# method(:+@), method(:-@) and method(:~) answer the receiver's unary
# operator: the desugar gave a non-letter name a wrapper only when it was a
# binary operator, so these raised NoMethodError (NameError for Integer#+@,
# which Integer inherits from Numeric) or, on a Float, failed to compile.
m = 5.method(:+@)
p m.call, m.arity, m.owner, m.name
n = 5.method(:-@)
p n.call, n.arity, n.owner
t = 7.method(:~)
p t.call, t.arity, t.owner
p 2.5.method(:-@).call, 2.5.method(:+@).call, 2.5.method(:-@).owner, 2.5.method(:+@).owner
p "ab".method(:-@).call, "ab".method(:+@).call.frozen?
begin
  5.method(:-@).call(1)
rescue ArgumentError => e
  p e.message
end
