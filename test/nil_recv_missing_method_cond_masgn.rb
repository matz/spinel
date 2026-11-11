# A call on nil to a method nil lacks raises NoMethodError wherever it
# stands: as the value of a multiple assignment (no target is written), as
# a condition, as an operand of `&&` or `||`, and through a chain. Only the
# plain assignment compiled; the others were refused (#11614).
def lookup(name) = nil

def run
  yield
rescue NoMethodError => e
  puts "NoMethodError: #{e.message}"
end

a = :a0
b = :b0
run do
  a, b = lookup("x").fetch_pair
  puts "not reached"
end
p [a, b]

run do
  account = lookup("x")
  c, d = account.pair
  p [c, d]
end

run do
  account = lookup("x")
  puts(account.work ? :manager : :member)
end

run do
  account = lookup("x")
  if account.work && account.work.manager
    puts :manager
  end
end

run do
  account = lookup("x")
  puts :none if account.work || account.other
end

run do
  account = lookup("x")
  puts :idle unless account.busy
end
