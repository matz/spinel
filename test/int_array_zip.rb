# Array#zip with a block on int_arrays. Both block params must shadow
# outer same-named locals of a different C type. Body uses a counter
# so this isolates the scope-shadow case from any param-read path.

a = "ha"
b = "hb"
puts a
puts b
foo = [1, 2, 3]
bar = [10, 20, 30]
n = 0
foo.zip(bar) { |a, b| n = n + 1 }
puts n   # 3
