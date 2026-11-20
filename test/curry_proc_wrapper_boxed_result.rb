# frozen_string_literal: true
class Answer
  def to_int
    raise "conversion must not run"
  end
end
def invoke(x, &blk)
  x.yield(2, &blk)
end
old = proc { |x, &b| b ? b.call(x) : 0 }
c = old.curry
$answer = Answer.new
v = invoke(c.to_proc) { |x| p x; $answer }
p v.equal?($answer)
