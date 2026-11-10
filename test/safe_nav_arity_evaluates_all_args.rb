# A `&.` call with more arguments than the count it takes evaluates every
# argument, in order, before ArgumentError is raised for a non-nil receiver,
# and none of them for a nil one. The count of arguments the call evaluates
# ahead of the raise has no limit: nine and twelve arguments are evaluated as
# three are.
$log = []
def a(i) = ($log << i; i)

def run(label)
  $log = []
  r = begin
    yield
  rescue ArgumentError => e
    "AE: #{e.message}"
  end
  puts "#{label}: #{r.inspect} evaluated #{$log.inspect}"
end

# a boxed receiver: each element has another class
boxed = ["str", [1], nil, :sym, {k: 1}]
boxed.each do |x|
  run("boxed 3") { x&.empty?(a(1), a(2), a(3)) }
  run("boxed 9") { x&.empty?(a(1), a(2), a(3), a(4), a(5), a(6), a(7), a(8), a(9)) }
  run("boxed 12") { x&.empty?(a(1), a(2), a(3), a(4), a(5), a(6), a(7), a(8), a(9), a(10), a(11), a(12)) }
end

# a receiver of one class
s = "str"
n = nil
run("string 9") { s&.empty?(a(1), a(2), a(3), a(4), a(5), a(6), a(7), a(8), a(9)) }
run("string 12") { s&.empty?(a(1), a(2), a(3), a(4), a(5), a(6), a(7), a(8), a(9), a(10), a(11), a(12)) }
run("nil 9") { n&.empty?(a(1), a(2), a(3), a(4), a(5), a(6), a(7), a(8), a(9)) }
arr = [1]
run("array 9") { arr&.empty?(a(1), a(2), a(3), a(4), a(5), a(6), a(7), a(8), a(9)) }
run("array 12") { arr&.empty?(a(1), a(2), a(3), a(4), a(5), a(6), a(7), a(8), a(9), a(10), a(11), a(12)) }
