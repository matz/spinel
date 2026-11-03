# A user exception class that defines the name of a reopened Object's private
# method answers it, also when the exception is raised and then rescued
# (boxed as an exception). Other exceptions raise NoMethodError.
class Object; private def top = "t"; end
class E < StandardError; def top = "e"; end
class G < StandardError; end
[E.new("a"), G.new("b"), RuntimeError.new("c"), 3].each { |v| p((v.public_send(:top) rescue "NE")); p((v.top rescue "NE")) }
[[E, "z"], [G, "w"], [RuntimeError, "r"]].each do |k, m|
  begin; raise k, m; rescue => ex; p((ex.top rescue "NE")); p((ex.public_send(:top) rescue "NE")); end
end
