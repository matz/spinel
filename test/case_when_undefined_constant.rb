# An undefined constant in when or === raises only if its read is reached,
# with the same NameError message as a plain read, including scoped paths.
def report
  p yield
rescue NameError => e
  puts e.message
end

def kind(v)
  case v
  when String then "string"
  when Nope then "nope"
  else "other"
  end
end
report { kind("s") }
report { kind(nil) }

# A statement case and a boxed subject keep the read as well.
def statement_kind(v)
  case v
  when String then puts "string statement"
  when Nope then puts "nope statement"
  else puts "other statement"
  end
  "done"
end
report { statement_kind("s") }
report { statement_kind(1) }
report { statement_kind(nil) }

def boxed_kind(v)
  case v
  when String then "string boxed"
  when Nope then "nope boxed"
  else "other boxed"
  end
end
report { boxed_kind("s") }
report { boxed_kind(1) }
report { boxed_kind(nil) }

# Each condition short-circuits too, not just each when clause.
def alternatives(v)
  case v
  when String, Nope then "matched"
  else "other"
  end
end
report { alternatives("s") }
report { alternatives(nil) }

def f(v) = Nope === v
report { f(nil) }
report { f(1) }
report { Nope === "s" }
report { Nope === (puts "unreached argument"; nil) }

module A; end
def path_kind(v)
  case v
  when Integer then "integer"
  when A::Nope then "nope path"
  else "other path"
  end
end
report { path_kind(1) }
report { path_kind(nil) }
report { A::Nope === nil }
report { A::Missing::Leaf === nil }
report { MissingParent::Leaf === nil }

# A splatted missing constant is read too, in a statement case as in an
# expression one.
def splat_statement(v)
  case v
  when *Nope then puts "nope splat"
  else puts "other splat"
  end
  "done"
end
report { splat_statement(1) }
report { case 1 when *Nope then :a else :b end }

# A path on a Class value whose leaf no class defines raises whatever the
# value is, naming it; a leaf some class defines still matches at run time.
module K; class Bar; end; end
def dynamic_kind(m, v)
  case v
  when m::Nope then "nope dynamic"
  else "other dynamic"
  end
end
def dynamic_statement(m, v)
  case v
  when m::Nope then puts "nope dynamic statement"
  else puts "other dynamic statement"
  end
  "done"
end
def dynamic_bar(m, v)
  case v
  when m::Bar then "bar"
  else "not bar"
  end
end
report { dynamic_kind(String, 1) }
report { dynamic_statement(K, 1) }
report { dynamic_bar(K, K::Bar.new) }
report { dynamic_bar(K, 1) }

# A missing date require must not look like a non-matching class.
report do
  case nil
  when Date then "date"
  else "not date"
  end
end

p(defined?(Nope) && Nope === nil)
p(defined?(A::Nope) && A::Nope === nil)
p(defined?(Nope) && kind(nil))

# Defined classes, value constants and the other condition forms still match.
def known_kind(v)
  case v
  when Later then "later"
  when TOKEN then "token"
  when *[7, 8] then "splat"
  when 1..3 then "range"
  when ->(x) { x == 9 } then "lambda"
  else "other known"
  end
end
class Later; end
TOKEN = :token
p known_kind(Later.new)
p known_kind(:token)
p known_kind(7)
p known_kind(2)
p known_kind(9)
p known_kind(nil)
p(Later === Later.new)
p(String === "s")
