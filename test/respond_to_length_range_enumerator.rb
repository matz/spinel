# Range and Enumerator get their names from Enumerable, which has no
# `length` (or `empty?`): only Array and Hash answer those.
r = (1..3)
p r.respond_to?(:length), r.respond_to?(:size), r.respond_to?(:count)
p (1.0..2.0).respond_to?(:length), ("a".."b").respond_to?(:length)
e = [1, 2].each
p e.respond_to?(:length), e.respond_to?(:size), e.respond_to?(:empty?), e.respond_to?(:next)
p [1].respond_to?(:length), { a: 1 }.respond_to?(:length), [].respond_to?(:empty?)
xs = [[1], { a: 1 }, (1..2), [1].each, "s", :s]
p xs.map { |v| v.respond_to?(:length) }
p xs.map { |v| v.respond_to?(:empty?) }
