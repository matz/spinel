# A `when` value of another family than the subject is never matched, but
# it is still evaluated first: one whose operator raises raises.
def f(x)
  case x
  when 1 / 0 then "int"
  else "none"
  end
rescue ZeroDivisionError => e
  "raised #{e.class}"
end
p f("x")
p f(:sym)

def g(x, z)
  case x
  when 5 % z then "int"
  else "none"
  end
rescue ZeroDivisionError => e
  "raised #{e.class}"
end
p g("y", 0)
p g("y", 3)

def h(x)
  case x
  when 1.0 / 0 then "float"
  else "none"
  end
end
p h("x")

def k(x, e)
  case x
  when 2 ** e then "int"
  else "none"
  end
end
p k("x", 2)

z = 0
case "y"
when 5 / z then p :int
else p :none
end rescue p $!.class

case 3
when "a" + "b" then p :str
when :s then p :sym
else p :none
end

case "s"
when 4 + 5 then p :int
when 2.5 * 2 then p :float
else p :none
end

def m(n, z)
  case n
  when "x" * (1 / z) then "str"
  else "none"
  end
rescue ZeroDivisionError => e
  "raised #{e.class}"
end
p m(3, 0)
p m(3, 1)
