begin
  [1, 2, 3].fetch(true)
rescue => e
  p [e.class, e.message]
end
begin
  [1, 2, 3].fetch(false)
rescue => e
  p [e.class, e.message]
end
# A statically Bool method result still needs evaluating, after the receiver.
def receiver
  print "R"
  [1, 2]
end
def bad_index
  print "K"
  false
end
begin
  receiver.fetch(bad_index)
rescue => e
  p [e.class, e.message]
end
# The other index readers share the guard.
begin
  [1, 2].at(true)
rescue => e
  p [e.class, e.message]
end
begin
  [1, 2][false]
rescue => e
  p [e.class, e.message]
end
begin
  [1, 2].values_at(true)
rescue => e
  p [e.class, e.message]
end
begin
  [{ b: 1 }].dig(false, :b)
rescue => e
  p [e.class, e.message]
end
