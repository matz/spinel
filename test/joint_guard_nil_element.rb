# A Float array with a gap, which reads nil: the update of the nil element raises
# NoMethodError, after the update ahead of it stored.
def upd(a)
  i = 0
  while i < 1
    a[i] += 1.0
    a[i + 1] += 1.0
    i += 1
  end
end
a = [1.0]
a[2] = 3.0
begin
  upd(a)
rescue NoMethodError => e
  p e.class
end
p a
