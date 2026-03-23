# send_cfunc_block benchmark (from yjit-bench)
arr = Array.new

i = 0
while i < 5000000
  arr.each { 0 }
  arr.each { 0 }
  arr.each { 0 }
  arr.each { 0 }
  arr.each { 0 }
  arr.each { 0 }
  arr.each { 0 }
  arr.each { 0 }
  arr.each { 0 }
  arr.each { 0 }
  i = i + 1
end
puts "done"
