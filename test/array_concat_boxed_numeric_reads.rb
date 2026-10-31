# spinel: gc-stress
# Boxed numeric Arrays keep nil flags and shifted storage when read by concat.
def concat_numeric_statement(a, b)
  a.concat(b)
  a
end
def concat_numeric_value(a, b)
  a.concat(b)
end
ints = [1, 2, 3]
ints[1] = nil
ints.shift
floats = [1.0, 2.0, 3.0]
floats[1] = nil
floats.shift
bi = [ints, 0].first
bf = [floats, 0].first
p concat_numeric_statement([0], bi)
p concat_numeric_value([0], bi)
p concat_numeric_statement([0.0], bf)
p concat_numeric_value([0.0], bf)
p concat_numeric_statement([0, "s"], bi)
p concat_numeric_value([0, "s"], bf)
