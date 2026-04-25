arr = [1, 2, 3, 4, 5]
s = arr.shuffle
puts s.length
puts s.sort.join(",")
puts arr.join(",")

words = ["foo", "bar", "baz", "qux"]
s2 = words.shuffle
puts s2.length
puts s2.include?("foo")
puts s2.include?("bar")
puts s2.include?("baz")
puts s2.include?("qux")
puts words.join(",")

nums = [10, 20, 30]
nums.shuffle!
puts nums.length
puts nums.sort.join(",")
