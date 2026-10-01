# A blockless map, collect, select or reject on a boxed value that has no
# such method (a String, an Integer, nil) raises NoMethodError naming the
# method, as CRuby does; an Array there still answers an Enumerator. The
# boxed form built an empty Enumerator over any value.
def pick(i) = ["str", [1, 2], 5, nil][i]
p pick(1).collect.to_a
[0, 2, 3].each do |i|
  begin
    pick(i).collect
    p :no_raise
  rescue NoMethodError
    p :no_method
  end
end
begin
  pick(0).reject
rescue NoMethodError => e
  p e.message
end
