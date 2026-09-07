# Array#reduce / Array#inject walk a receiver held only in a temporary and
# rebuild the accumulator from a freshly allocated value on every turn, so
# both slots have to stay live across the block. Every receiver here is the
# return value of a method call, so nothing else holds it -- except the one
# arm that deliberately holds it in a local and then rebinds that local
# inside the block. Every block allocates BEFORE it reads the accumulator it
# was handed, and every arm prints the accumulated content as well as the
# number of turns, so an arm that loses either slot shows up as a changed
# answer rather than as a passing test.

N = 40
CHURN = 100

def churn
  CHURN.times { "q" * 64 }
end

def strs
  (1..N).map { |i| "a#{i}" }
end

def ints
  (1..N).to_a
end

def nested
  [[1, 2, 3, 4], [2, 3, 4, 5], [3, 4, 5, 6]]
end

def four
  ["p", "q", "r", "s"]
end

def mix(acc, e)
  e.even? ? "s#{e}" : acc
end

class Box
  attr_reader :n

  def initialize(n)
    @n = n
  end

  def plus(k)
    Box.new(@n + k)
  end
end

# --- a seed the block rebuilds: String, Array, Hash, object, nil ---

turns = 0
s = strs.reduce("") do |acc, x|
  churn
  turns += 1
  acc + x
end
puts "reduce str seed turns=#{turns} len=#{s.size} head=#{s[0, 4]}"

turns = 0
a = ints.reduce([9]) do |acc, x|
  churn
  turns += 1
  acc + [x]
end
puts "reduce array seed turns=#{turns} len=#{a.size} sum=#{a.sum}"

turns = 0
h = ints.reduce({}) do |acc, x|
  churn
  turns += 1
  acc[x] = "v#{x}"
  acc
end
puts "reduce empty hash turns=#{turns} len=#{h.size} last=#{h[N]}"

turns = 0
e = ints.reduce([]) do |acc, x|
  churn
  turns += 1
  acc << x
end
puts "reduce empty array turns=#{turns} len=#{e.size} sum=#{e.sum}"

turns = 0
b = ints.reduce(Box.new(0)) do |acc, x|
  churn
  turns += 1
  acc.plus(x)
end
puts "reduce object seed turns=#{turns} n=#{b.n}"

turns = 0
n = strs.reduce(nil) do |acc, x|
  churn
  turns += 1
  acc.nil? ? x : acc + x
end
puts "reduce nil seed turns=#{turns} len=#{n.size} head=#{n[0, 4]}"

turns = 0
i = ints.reduce(0) do |acc, x|
  churn
  turns += 1
  acc + x
end
puts "reduce int seed turns=#{turns} sum=#{i}"

# --- no seed: the accumulator starts at element 0 and is rebuilt from there ---

turns = 0
u = strs.reduce do |acc, x|
  churn
  turns += 1
  acc + x
end
puts "reduce no seed turns=#{turns} len=#{u.size} head=#{u[0, 4]}"

turns = 0
m = ints.reduce do |acc, x|
  churn
  turns += 1
  mix(acc, x)
end
puts "reduce no seed boxed turns=#{turns} out=#{m.inspect}"

turns = 0
k = nested.reduce do |acc, x|
  churn
  turns += 1
  acc & x
end
puts "reduce nested turns=#{turns} out=#{k.inspect}"

# --- the same walk under the #inject spelling ---

turns = 0
j = ints.inject("") do |acc, x|
  churn
  turns += 1
  acc + x.to_s
end
puts "inject str seed turns=#{turns} len=#{j.size} head=#{j[0, 4]}"

turns = 0
w = strs.inject do |acc, x|
  churn
  turns += 1
  acc + x
end
puts "inject no seed turns=#{turns} len=#{w.size} head=#{w[0, 4]}"

# A receiver held in a named local is not enough on its own: the block can
# reassign that local, and the walk still belongs to the array the fold
# started on.

def rebound
  list = strs
  turns = 0
  out = list.reduce("") do |acc, x|
    churn
    turns += 1
    list = []
    acc + x
  end
  "turns=#{turns} len=#{out.size} head=#{out[0, 4]}"
end
puts "reduce rebound receiver #{rebound}"

# The fold is a statement expression, so its roots are pushed and popped where
# the expression is evaluated: inside another fold's block rather than ahead of
# it, and once per turn of an enclosing loop.

turns = 0
nest = four.reduce("") do |acc, x|
  churn
  turns += 1
  acc + strs.reduce("") { |a, y| churn; a + y[0] }[0, 1]
end
puts "nested folds turns=#{turns} len=#{nest.size}"

rounds = 0
while strs.reduce("") { |acc, x| churn; acc + x }.size == 111 && rounds < 3
  rounds += 1
end
puts "fold in a while condition rounds=#{rounds}"

# A fold suspended inside a Fiber keeps both slots across the switch: the
# collection happens on the main side while the fold is parked mid-walk.

fib = Fiber.new do
  strs.reduce("") do |acc, x|
    Fiber.yield acc.size if x == "a5"
    acc + x
  end
end
part = fib.resume
20.times { churn }
puts "fiber fold part=#{part} rest=#{fib.resume.size}"

# --- leaving the walk early: break, next, return, raise ---

br = strs.reduce("") do |acc, x|
  churn
  break "B#{acc.size}" if x == "a5"
  acc + x
end
puts "break out=#{br}"

nx = strs.reduce("") do |acc, x|
  churn
  next acc if x.size == 3
  acc + x
end
puts "next len=#{nx.size}"

def early(list)
  list.reduce("") do |acc, x|
    churn
    return "R#{acc.size}" if x == "a4"
    acc + x
  end
end
puts "return out=#{early(strs)}"

def boom(list)
  list.reduce("") do |acc, x|
    churn
    raise "stop#{acc.size}" if x == "a6"
    acc + x
  end
end
begin
  boom(strs)
rescue RuntimeError => ex
  puts "raise out=#{ex.message}"
end

rin = strs.reduce("") do |acc, x|
  churn
  begin
    raise "inner"
  rescue RuntimeError
    acc + x
  end
end
puts "rescue inside len=#{rin.size}"

# The walk's answer has to survive one more allocating step after the fold.
tail = strs.reduce("") do |acc, x|
  churn
  acc + x
end
churn
puts "tail len=#{tail.size} head=#{tail[0, 4]}"

# Every early exit pops the roots it pushed: 60_000 folds that leave through
# break, return and raise, then one more full fold. A leaked push would pass
# SP_GC_STACK_MAX and the last fold would answer short.
def esc(list)
  list.reduce("") { |acc, x| return "R" if x == "r"; acc + x }
end

def bang(list)
  list.reduce("") { |acc, x| raise "x" if x == "r"; acc + x }
end

seen = 0
60_000.times do |round|
  out = case round % 3
        when 0 then four.reduce("") { |acc, x| break "B" if x == "r"; acc + x }
        when 1 then esc(four)
        else (begin; bang(four); rescue RuntimeError; "E"; end)
        end
  seen += out.size
end
last = strs.reduce("") do |acc, x|
  churn
  acc + x
end
puts "after 60000 exits seen=#{seen} last=#{last.size}"
