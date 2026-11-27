# A Mutex#synchronize or a select! in a proc whose body ends in nil: the
# region declared its deferred return `void _retvN`, which C refuses, where
# a begin..ensure in the same place declares none (#4245). A Rails
# after-commit hook is written this way: a lambda that calls the helper.
M = Mutex.new
ACTIVITY = {}
HOOKS = []

def with_activity
  M.synchronize { yield ACTIVITY }
end

def note(id, at)
  with_activity { |h| h[id] = at }
end

def flush
  pending = ACTIVITY.dup
  HOOKS << -> {
    with_activity do |h|
      pending.each { |id, at| h.delete(id) if h[id] == at }
    end
    nil
  }
  pending.size
end

note(1, 10)
note(2, 20)
p flush
HOOKS.each(&:call)
p ACTIVITY.size

locked = proc {
  M.synchronize { puts "locked" }
  nil
}
p locked.call

list = [1, 2, 3, 4]
evens = -> {
  list.select! { |x| x.even? }
  nil
}
p evens.call
p list
