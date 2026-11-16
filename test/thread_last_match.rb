# spinel: gc-stress
# `$~` is thread-local: a thread reads its own match after another thread
# ran a match. The two threads take turns through queues, so each one runs
# its match while the other waits between its own match and its reads.
def turn(text, pattern, mine, other)
  mine.pop
  text =~ pattern
  other << :go
  mine.pop
  r = [$~[0], $1, $~.pre_match]
  other << :go
  r
end

qa = Queue.new
qb = Queue.new
a = Thread.new { turn("xaay", /(a)a/, qa, qb) }
b = Thread.new { turn("zzbbw", /(b)b/, qb, qa) }
qa << :go
p a.value
p b.value

# a new thread starts with no match, and its match leaves the one of the
# thread that started it
"main" =~ /(m)a/
t = Thread.new do
  r = [$~]
  "thread" =~ /(t)h/
  r << $1
end
p t.value
p [$~[0], $1]
