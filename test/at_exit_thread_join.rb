# The exit drain shut the helper workers down ahead of the at_exit hooks, so a
# thread a hook started had no worker to run it, and a hook that joined it
# waited forever. CRuby runs the hooks while the program's threads are alive.
sleeper = Thread.new do
  sleep 0.02
  puts "a thread main left sleeping woke"
end

at_exit do
  # reached last: hooks run in reverse order
  puts "first hook"
end

at_exit do
  t = Thread.new { 40 + 2 }
  puts "value from a hook's thread: #{t.value}"

  q = Queue.new
  Thread.new { q.push("pushed") }
  puts "queue filled by a hook's thread: #{q.pop}"

  sleeper.join
  puts "last hook"
end

puts "main done"
