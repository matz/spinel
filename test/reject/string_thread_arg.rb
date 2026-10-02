# A String variable on this route must not silently lose its append.
s = +"a"
Thread.new(s) { |t| t << "!" }.join
p s
