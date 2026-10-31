# spinel: gc-stress
# The nested Array's out-of-range read is a typed Array pointer whose NULL
# is nil. It must not become an empty source through an Array conversion.
rows = [[3]]
x = rows[2]
a = [1]
begin
  a.concat(x)
rescue => e
  p e.class
end
p a
a = [1]
begin
  r = a.concat(x)
  p r.equal?(a)
rescue => e
  p e.class
end
p a
a = [1].freeze
begin
  a.concat(x)
rescue => e
  p e.class
end
