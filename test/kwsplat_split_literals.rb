# Literal `**{}`, `**nil` and `**{a: 1}` operands.
class R < Array
  alias_method :pu, :push
  alias_method :s, :sum
end
p R.new.pu(**{ a: 1 })
p R.new.pu(1, **{ "s" => 1 })
p R.new.pu(1, **{ a: 1 }, **{})
p((R.new.push(1).s(**{ a: 1 }) rescue $!.class))
p R.new.pu(1, **nil)
e = {}
p R.new.pu(1, **e, **{ b: 2 })
p R.new.pu(1, k: 2)
