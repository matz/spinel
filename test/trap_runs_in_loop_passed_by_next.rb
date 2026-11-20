# A loop polls for a delivered signal between two of its passes: after the
# body, and on each jump to the next pass. Each loop below ends every pass but
# the last with a `next` (or a `redo`, or a `next` through an ensure), and
# leaves only once the trap has run, which it can do only inside the loop: a
# loop whose poll the jump skipped would spin for good.
$stdout.sync = true
$hits = 0
Signal.trap(:USR1) { $hits += 1 }

def kick
  Process.spawn("sh", "-c", "sleep 0.05; kill -USR1 #{Process.pid}")
end

def while_next
  seen = $hits
  k = kick
  n = 0
  x = 0.0
  while true
    n += 1
    x = x + 1.5
    next if $hits == seen
    break
  end
  Process.wait(k)
  [n > 0, x > 0.0]
end

def until_next
  seen = $hits
  k = kick
  done = false
  until done
    next if $hits == seen
    done = true
  end
  Process.wait(k)
  done
end

def loop_next
  seen = $hits
  k = kick
  n = 0
  loop do
    n += 1
    next unless $hits > seen
    break
  end
  Process.wait(k)
  n > 0
end

def next_through_ensure
  seen = $hits
  k = kick
  ran = 0
  while true
    begin
      next if $hits == seen
      break
    ensure
      ran += 1
    end
  end
  Process.wait(k)
  ran > 0
end

def each_redo
  seen = $hits
  k = kick
  passes = 0
  [1].each do |v|
    passes += v
    redo if $hits == seen
  end
  Process.wait(k)
  passes > 0
end

p while_next
p until_next
p loop_next
p next_through_ensure
p each_redo
p $hits
