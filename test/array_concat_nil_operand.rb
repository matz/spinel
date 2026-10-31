def maybe_array(b)
  b ? [3] : nil
end
a = [1]
x = maybe_array(false)
begin
a.concat(x)
rescue => e
  p e.class
end
p a
