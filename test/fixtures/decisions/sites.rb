# One site of each decision kind a small program reaches: a root frame and a
# forced inline per method, an operand that cannot allocate beside a hash
# store, a `case` subject, a multiple assignment, a fetch with an inert key
# and default, and a global lent to a method that appends to it, which no
# answer about an operand may turn into a copy.
def first_word(s)
  parts = s.split(" ")
  parts[0]
end

def last_word(s)
  parts = s.split(" ")
  parts[parts.length - 1]
end

def grow(s, n)
  s << "!" * n
end

h = {}
k = "a b"
h[k] = first_word(k)
case h[k]
when "a" then puts "first " + last_word(k)
else puts "other"
end
x, y = k, 2
puts x, y
puts h.fetch(k, "none")
n = {}
n[k] = 1
puts n.fetch(k, 0)
$log = +"log"
grow($log, 3)
puts $log
