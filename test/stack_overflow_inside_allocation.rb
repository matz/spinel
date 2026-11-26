# A recursion that allocates at each level overflows the C stack inside the
# allocator. The fault handler raises an overflow by a longjmp out of the
# frame that faulted, which left malloc's lock held when the fault was in
# malloc, and the next malloc, from the rescue clause, aborted the process.
# The allocator's slow paths raise SystemStackError themselves with a margin
# of stack left, so the rescue runs. A Fiber and a Thread run on a 256 KB
# stack: the String that grows by one at each level is what put the fault in
# malloc, and stays small at that depth. The main stack is 64 MB deep, so a
# level that allocates there runs a collection per level under GC stress,
# each over the whole stack: too slow for a test, and the bound it reads is
# the one the fault handler reads.
def deep(n, s) = deep(n + 1, s + "x") + 1
p Fiber.new { begin; deep(0, ""); rescue SystemStackError => e; "fiber: #{e.class}"; end }.resume
t = Thread.new { begin; deep(0, ""); rescue SystemStackError => e; "thread: #{e.class}"; end }
p t.value
