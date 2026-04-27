# Array#each on a str_array. The block param must shadow an outer
# same-named mrb_int local. Body skips `puts i` to keep this scope-
# shadow case separate from the format-specifier path.

3.times do |i|
  puts i
end
foo = ["a", "b", "c"]
n = 0
foo.each { |i| n = n + 1 }
puts n   # 3
