# An empty interpolation `#{}` is nil, whose to_s writes nothing.
x = "a#{}"
puts x
def label(n) = "alias #{n} : #{} is #{n * 2} #{};"
puts label(3)
puts "#{}".empty?
puts "#{ }b#{}c".size
s = :"sym#{}bol"
p s
y = "pre"
y = "#{y}#{}post"
puts y
puts <<~T
  head #{} tail
T
puts `echo #{}x`
