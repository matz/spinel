# A call in a method nothing calls is dropped with it by codegen, so it emits
# no reinterpreting call. Judged as a call site anyway, an argument its
# untyped caller left as a String refused a program CRuby runs.
class DeadCallerCounter
  def bump(n)
    n + 1
  end
end

def dead_caller_bump
  DeadCallerCounter.new.bump("x")
end

puts DeadCallerCounter.new.bump(1)
