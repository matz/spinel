# A boxed operand with no such operator raises NoMethodError with the
# receiver and the operand, as a compiled call does (#10854 under promote).
def op(a, b) = a - b
[[nil, 1], [:s, 2], [true, 3]].each do |a, b|
  begin; op(a, b); rescue NoMethodError => e; p [e.name, e.receiver, e.args]; end
end
