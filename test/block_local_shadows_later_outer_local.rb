# A name first assigned inside a block is the block's own local, even when the
# enclosing scope assigns the same name further down: Ruby scopes a block's
# body by what is declared textually before it. A paramless block skipped the
# slot of its own, so its local shared one slot with the later top-level
# assignment -- every thread of `Thread.new do y = ...; blk { y } end` read
# the last writer's value (found under #4528).
ths = Array.new(3) { |i| Thread.new { y = i; sleep 0.01 * (3 - i); [0].map { y }.first } }
p ths.map(&:value)
y = 7
p y

procs = Array.new(3) { |i| proc { z = i * 2; -> { z }.call } }
p procs.map(&:call)
z = "outer"
p z

loop do
  w = :inner
  p w
  break
end
w = :outer
p w

# a name assigned BEFORE the block is the outer one, shared as ever
seen = 0
[1, 2].each { seen += 1 }
Thread.new { seen += 10 }.join
p seen
