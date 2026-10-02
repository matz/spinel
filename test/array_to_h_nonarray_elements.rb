begin
  p [1, 2].to_h
rescue => e
  p [e.class, e.message]
end
begin
  p ["x"].to_h
rescue => e
  p [e.class, e.message]
end
begin
  p [1.5].to_h
rescue => e
  p [e.class, e.message]
end
# Empty typed storage is valid even though populated primitive storage isn't.
a = [1]
a.pop
p a.to_h
