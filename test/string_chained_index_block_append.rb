# Chained Array iterators may read their String elements.
a = ["a", "b"]
a.each.with_index { |q, i| p [q, i] }
a.map.with_index { |q, i| p q + i.to_s }
a.each.each_with_index { |q, i| p [q, i] }
