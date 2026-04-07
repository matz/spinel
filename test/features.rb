# Test various Ruby features

# each_with_index
[10, 20, 30].each_with_index { |v, i|
  puts i.to_s + ":" + v.to_s
}

# for..in array
sum = 0
for x in [1, 2, 3, 4, 5]
  sum = sum + x
end
puts sum

# for..in range
s = ""
for i in 1..5
  s = s + i.to_s
end
puts s

# String#tr
puts "hello".tr("el", "ip")

# String#ljust / rjust
puts "hi".ljust(6) + "|"
puts "hi".rjust(6) + "|"

# global variable type check (same type OK)
$g = 10
$g = 20
puts $g
