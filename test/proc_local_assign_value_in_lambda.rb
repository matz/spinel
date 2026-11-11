# `if (q = b)` inside a lambda, where q is a Proc local of the enclosing
# method: the assignment's value form writes the captured slot (its read
# form is a cast, which C cannot assign to).
def mk
  q = proc { 0 }
  l = lambda do |x, &b|
    if (q = b)
      q.call + x
    else
      -1
    end
  end
  [l, q]
end

l, q = mk
p q.call
p l.call(1) { 41 }
p l.call(1)

def mk2
  q = nil
  l = lambda { |&b| (q = b) && q.call }
  [l, -> { q }]
end

l2, get = mk2
p l2.call { :given }
p get.call.call
p l2.call
p get.call
