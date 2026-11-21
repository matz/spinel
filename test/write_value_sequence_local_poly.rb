# `x = begin; c = <a value of several kinds>; c; end`: the local `c` is
# written at a higher node id than `x`, and every round of the write-type
# pass resets locals and walks writes in node order, so `x` read `c` reset
# and stayed unknown through the whole fixpoint. Only the late widening made
# it poly, after the passes that decide on a receiver's type had run, and
# `x.n ||= 3` was refused. Liquid's Template#render binds `c = args.shift`
# in a `case` arm and answers it.

class Ctx
  attr_accessor :n
end

def from_case(*args)
  x = case args.first
      when Ctx
        c = args.shift
        c
      else
        Ctx.new
      end
  x.n ||= 3
  x.n
end

def from_begin(k)
  x = begin
    c = k > 2 ? 1 : Ctx.new
    c
  end
  x.n ||= 4
  x.n
end

def from_if(args)
  x = if args.empty?
        Ctx.new
      else
        c = args.shift
        c
      end
  x.n ||= 5
  x.n
end

p from_case
p from_case(Ctx.new)
p from_case({ "a" => 1 })
p from_begin(1)
p from_if([])
p from_if([Ctx.new])
