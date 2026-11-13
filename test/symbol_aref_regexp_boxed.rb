# sym[/re/, group] on a boxed Symbol matches the name of the Symbol, as on
# a typed one: a numbered group, a named group, and no match
def t
  yield
rescue => e
  "#{e.class}: #{e.message}"
end
box = [:hello, "hello", 1]
sym = box[0]
p t { sym[/(?<x>l+)/, "x"] }
p t { sym[/(?<x>l+)/, :x] }
p t { sym[/(h)(e)/, 2] }
p t { sym[/(h)(e)/, 0] }
p t { sym[/(h)(e)/, -1] }
p t { sym[/(?<x>z+)/, "x"] }
p t { sym[/z/, 1] }
p t { sym[1, 2] }
# the String beside it in the same Array takes the same call
p t { box[1][/(?<x>l+)/, "x"] }
box.first(2).each { |v| p t { v[/(l)(l)/, 1] } }
